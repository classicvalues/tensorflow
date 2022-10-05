/* Copyright 2022 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/memory/memory.h"
#include "absl/time/time.h"
#include "tensorflow/core/distributed_runtime/coordination/coordination_client.h"
#include "tensorflow/core/distributed_runtime/coordination/coordination_service.h"
#include "tensorflow/core/distributed_runtime/coordination/coordination_service_agent.h"
#include "tensorflow/core/distributed_runtime/rpc/async_service_interface.h"
#include "tensorflow/core/distributed_runtime/rpc/coordination/grpc_coordination_client.h"
#include "tensorflow/core/distributed_runtime/rpc/coordination/grpc_coordination_service_impl.h"
#include "tensorflow/core/platform/env.h"
#include "tensorflow/core/platform/status.h"
#include "tensorflow/core/platform/test.h"
#include "tensorflow/core/platform/threadpool.h"
#include "tensorflow/core/protobuf/cluster.pb.h"
#include "tensorflow/core/protobuf/config.pb.h"
#include "tensorflow/core/protobuf/coordination_config.pb.h"
#include "tensorflow/core/protobuf/tensorflow_server.pb.h"
#include "tensorflow/tsl/lib/core/status_test_util.h"
#include "tensorflow/tsl/platform/mutex.h"
#include "tensorflow/tsl/protobuf/error_codes.pb.h"

namespace tensorflow {
namespace {

constexpr char kParameterServerJobName[] = "parameter_server";
constexpr char kWorkerJobName[] = "worker";
constexpr char kCoordinationServiceType[] = "standalone";
constexpr char kServiceLeader[] = "/job:parameter_server/replica:0/task:0";

class TestCoordinationClientCache : public CoordinationClientCache {
 public:
  void AddTask(const std::string& target, CoordinationClient* client) {
    mutex_lock l(clients_mu_);
    clients_.emplace(target, client);
  }

  CoordinationClient* GetClient(const string& target) override {
    mutex_lock l(clients_mu_);
    if (auto it = clients_.find(target); it != clients_.end()) {
      return it->second;
    }
    return nullptr;
  }

  std::unique_ptr<CoordinationClient> GetOwnedClient(
      const string& target) override {
    LOG(ERROR) << "GetOwnedClient is not supported.";
    return nullptr;
  }

 private:
  mutex clients_mu_;
  absl::flat_hash_map<std::string, CoordinationClient*> clients_
      TF_GUARDED_BY(clients_mu_);
};

class TestCoordinationServiceTaskState {
 public:
  TestCoordinationServiceTaskState() {}

  ~TestCoordinationServiceTaskState() {}

  void Shutdown() {
    coord_client_.reset();
    coord_agent_.reset();
    coord_compute_pool_.reset();
    grpc_server_->Shutdown();
    coord_rpc_service_->Shutdown();
  }

  void StartCoordinationService() {
    ::grpc::ServerBuilder builder;
    coord_compute_pool_ = std::make_unique<thread::ThreadPool>(
        Env::Default(), /*name=*/"CoordinationServiceRpcHandler",
        /*num_threads=*/5);
    coord_rpc_service_ = std::make_unique<GrpcCoordinationServiceImpl>(
        coord_compute_pool_.get(), &builder);
    auto* grpc_coord_service =
        static_cast<GrpcCoordinationServiceImpl*>(coord_rpc_service_.get());
    grpc_coord_service->SetCoordinationServiceAgentInstance(coord_agent_.get());
    grpc_server_ = builder.BuildAndStart();
    coord_client_ = absl::WrapUnique(NewGrpcCoordinationClient(
        grpc_server_->InProcessChannel(::grpc::ChannelArguments())));
    coord_rpc_thread_ = absl::WrapUnique(Env::Default()->StartThread(
        /*thread_options=*/{}, /*name=*/"CoordinationServiceHandleRPCsLoop",
        [service = coord_rpc_service_.get()]() { service->HandleRPCsLoop(); }));
  }

  void InitializeAndConnectCoordinationAgents(
      const std::string& job_name, int task_id,
      const CoordinationServiceConfig& coordination_config) {
    auto error_fn = [this, job_name](const Status& status) {
      this->status_ = status;
      LOG(ERROR) << "Coordination service agent of " << job_name
                 << " is in error status: " << status;
    };

    TF_CHECK_OK(coord_agent_->Initialize(Env::Default(), job_name, task_id,
                                         coordination_config,
                                         std::move(coord_client_), error_fn));
    TF_CHECK_OK(coord_agent_->Connect());
    TF_CHECK_OK(status_);
  }

  CoordinationClient* GetCoordinationClient() { return coord_client_.get(); }

  Status ReportError(const Status& status) {
    return coord_agent_->ReportError(status);
  }

  Status GetStatus() const { return status_; }

 private:
  std::unique_ptr<::grpc::Server> grpc_server_;
  std::unique_ptr<thread::ThreadPool> coord_compute_pool_;
  std::unique_ptr<AsyncServiceInterface> coord_rpc_service_;
  std::unique_ptr<Thread> coord_rpc_thread_;
  std::unique_ptr<CoordinationServiceAgent> coord_agent_ =
      CreateCoordinationServiceAgent();
  std::unique_ptr<CoordinationClient> coord_client_;
  Status status_;
};

class CoordinationServiceRecoverableJobTest : public ::testing::Test {
 public:
  void SetUp() override {
    state_ps_0_.StartCoordinationService();
    state_ps_1_.StartCoordinationService();
    state_worker_0_.StartCoordinationService();
    state_worker_1_.StartCoordinationService();
  }

  void TearDown() override {
    state_ps_0_.Shutdown();
    state_ps_1_.Shutdown();
    state_worker_0_.Shutdown();
    state_worker_1_.Shutdown();
    coord_service_.reset();
  }

  void Initialize() {
    ConfigureCoordinationService();
    auto client_cache = std::make_unique<TestCoordinationClientCache>();
    client_cache->AddTask(
        /*target=*/kServiceLeader, state_ps_0_.GetCoordinationClient());
    client_cache->AddTask(
        /*target=*/"/job:parameter_server/replica:0/task:1",
        state_ps_1_.GetCoordinationClient());
    client_cache->AddTask(
        /*target=*/"/job:worker/replica:0/task:0",
        state_worker_0_.GetCoordinationClient());
    client_cache->AddTask(
        /*target=*/"/job:worker/replica:0/task:1",
        state_worker_1_.GetCoordinationClient());
    coord_service_ = CoordinationServiceInterface::EnableCoordinationService(
        kCoordinationServiceType, Env::Default(), server_def_,
        std::move(client_cache));
    const auto& coordination_config = server_def_.default_session_config()
                                          .experimental()
                                          .coordination_config();
    state_ps_0_.InitializeAndConnectCoordinationAgents(kParameterServerJobName,
                                                       /*task_id=*/0,
                                                       coordination_config);
    state_ps_1_.InitializeAndConnectCoordinationAgents(kParameterServerJobName,
                                                       /*task_id=*/1,
                                                       coordination_config);
    state_worker_0_.InitializeAndConnectCoordinationAgents(kWorkerJobName,
                                                           /*task_id=*/0,
                                                           coordination_config);
    state_worker_1_.InitializeAndConnectCoordinationAgents(kWorkerJobName,
                                                           /*task_id=*/1,
                                                           coordination_config);
  }

  void ConfigureCoordinationService() {
    // Assume the coordination service is deployed in the parameter server.
    server_def_.set_protocol("grpc");
    server_def_.set_job_name(kParameterServerJobName);
    server_def_.set_task_index(0);

    auto ps_job_def = server_def_.mutable_cluster()->add_job();
    ps_job_def->set_name(kParameterServerJobName);
    (*ps_job_def->mutable_tasks())[0] = "test_address_ps_0";
    (*ps_job_def->mutable_tasks())[1] = "test_address_ps_1";

    auto worker_job_def = server_def_.mutable_cluster()->add_job();
    worker_job_def->set_name(kWorkerJobName);
    (*worker_job_def->mutable_tasks())[0] = "test_address_worker_0";
    (*worker_job_def->mutable_tasks())[1] = "test_address_worker_1";

    auto coordination_config = server_def_.mutable_default_session_config()
                                   ->mutable_experimental()
                                   ->mutable_coordination_config();
    coordination_config->set_service_type(kCoordinationServiceType);
    coordination_config->set_service_leader(kServiceLeader);
  }

  void AddJobToRecoverableJobs(const std::string& job_name) {
    server_def_.mutable_default_session_config()
        ->mutable_experimental()
        ->mutable_coordination_config()
        ->add_recoverable_jobs(job_name);
  }

 protected:
  ServerDef server_def_;
  std::unique_ptr<CoordinationServiceInterface> coord_service_;
  TestCoordinationServiceTaskState state_ps_0_;
  TestCoordinationServiceTaskState state_ps_1_;
  TestCoordinationServiceTaskState state_worker_0_;
  TestCoordinationServiceTaskState state_worker_1_;
};

TEST_F(CoordinationServiceRecoverableJobTest,
       UnrecoverableWorkerFailurePropagated) {
  Initialize();
  TF_ASSERT_OK(state_worker_0_.ReportError(errors::Internal("Test Error.")));

  // For unrecoverable task, error propagates to all connected tasks.
  EXPECT_TRUE(errors::IsInternal(state_ps_0_.GetStatus()));
  EXPECT_TRUE(errors::IsInternal(state_ps_1_.GetStatus()));
  EXPECT_TRUE(errors::IsInternal(state_worker_0_.GetStatus()));
  EXPECT_TRUE(errors::IsInternal(state_worker_1_.GetStatus()));
}

TEST_F(CoordinationServiceRecoverableJobTest,
       UnrecoverablePSFailurePropagated) {
  Initialize();
  TF_ASSERT_OK(state_ps_0_.ReportError(errors::Internal("Test Error.")));

  // For unrecoverable task, error propagates to all connected tasks.
  EXPECT_TRUE(errors::IsInternal(state_ps_0_.GetStatus()));
  EXPECT_TRUE(errors::IsInternal(state_ps_1_.GetStatus()));
  EXPECT_TRUE(errors::IsInternal(state_worker_0_.GetStatus()));
  EXPECT_TRUE(errors::IsInternal(state_worker_1_.GetStatus()));
}

TEST_F(CoordinationServiceRecoverableJobTest,
       RecoverableWorkerFailureNotPropagated) {
  AddJobToRecoverableJobs(kWorkerJobName);
  Initialize();
  TF_ASSERT_OK(state_worker_0_.ReportError(errors::Internal("Test Error.")));

  // For recoverable task, error does not propagate.
  EXPECT_TRUE(state_ps_0_.GetStatus().ok());
  EXPECT_TRUE(state_ps_1_.GetStatus().ok());
  EXPECT_TRUE(errors::IsInternal(state_worker_0_.GetStatus()));
  EXPECT_TRUE(state_worker_1_.GetStatus().ok());
}

}  // namespace
}  // namespace tensorflow
