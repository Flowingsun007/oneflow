/*
Copyright 2020 The OneFlow Authors. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/
#ifndef ONEFLOW_CORE_CONTROL_CTRL_CLIENT_H_
#define ONEFLOW_CORE_CONTROL_CTRL_CLIENT_H_

#include "oneflow/core/control/rpc_client.h"
#include "oneflow/core/rpc/include/ctrl.h"
#include "oneflow/core/control/ctrl_bootstrap.pb.h"

namespace oneflow {

class GrpcCtrlClient final : public CtrlClient {
 public:
  OF_DISALLOW_COPY_AND_MOVE(GrpcCtrlClient);
  GrpcCtrlClient(const ProcessCtx& process_ctx);
  ~GrpcCtrlClient();

  void Barrier(const std::string& barrier_name);
  void Barrier(const std::string& barrier_name, int32_t barrier_num);

  TryLockResult TryLock(const std::string& name);
  void NotifyDone(const std::string& name);
  void WaitUntilDone(const std::string& name);

  void PushKV(const std::string& k, std::function<void(std::string*)> VSetter);
  void PushKV(const std::string& k, const std::string& v);
  void PushKV(const std::string& k, const PbMessage& msg);
  void PushMasterKV(const std::string& k, const PbMessage& msg);

  void ClearKV(const std::string& k);
  void ClearMasterKV(const std::string& k);

  void PullKV(const std::string& k, std::function<void(const std::string&)> VGetter);
  void PullKV(const std::string& k, std::string* v);
  void PullKV(const std::string& k, PbMessage* msg);
  void PullMasterKV(const std::string& k, PbMessage* msg);
  void PushActEvent(const ActEvent&){};
  void Clear();

  int32_t IncreaseCount(const std::string& k, int32_t v);
  int32_t IncreaseCount(const std::string& k) { return IncreaseCount(k, 1); }
  void EraseCount(const std::string& k);

 protected:
  void PushMasterKV(const std::string& k, std::function<void(std::string*)> VSetter);
  void PullMasterKV(const std::string& k, std::function<void(const std::string&)> VGetter);

 private:
  const ProcessCtx& process_ctx() const { return process_ctx_; }
  ProcessCtx process_ctx_;
  bool need_heartbeat_thread_stop_;
  std::mutex need_heartbeat_thread_stop_mtx_;
  std::thread heartbeat_thread_;
  RpcClient rpc_client_;
};

}  // namespace oneflow

#endif  // ONEFLOW_CORE_CONTROL_CTRL_CLIENT_H_
