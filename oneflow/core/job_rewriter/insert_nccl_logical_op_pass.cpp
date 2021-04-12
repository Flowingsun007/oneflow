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
#include "oneflow/core/job_rewriter/job_pass.h"
#include "oneflow/core/job/job.pb.h"
#include "oneflow/core/job/scope.h"
#include "oneflow/core/job/sbp_parallel.h"
#include "oneflow/core/job_rewriter/calculation_pass.h"
#include "oneflow/core/vm/symbol_storage.h"
#include "oneflow/core/framework/framework.h"
#include "oneflow/core/operator/operator.h"
#include "oneflow/core/graph/boxing/hierarchical_sub_task_graph_builder_impl.h"

namespace oneflow {

namespace {

// Do InsertNcclLogicalOpPass will use backward recomputation for sublinear memory cost.
class InsertNcclLogicalOpPass final : public JobPass {
 public:
  OF_DISALLOW_COPY_AND_MOVE(InsertNcclLogicalOpPass);
  InsertNcclLogicalOpPass() = default;
  ~InsertNcclLogicalOpPass() = default;

  Maybe<void> Apply(Job* job, JobPassCtx* ctx) const override {
    if (!IsEnabled(*ctx)) { return Maybe<void>::Ok(); }
    const OpGraph op_graph(*job);
    JobBuilder job_builder(job);
    return Apply(op_graph, &job_builder);
  }

  bool IsEnabled(const JobPassCtx& ctx) const {
    return Global<ResourceDesc, ForSession>::Get()->nccl_use_compute_stream();
  }

  Maybe<void> Apply(const OpGraph& op_graph, JobBuilder* job_builder) const;
};

const std::string kNcclLogicalOpNamePrefix = "System-NCCL-Logical";

std::string ParallelDistributionToString(const ParallelDistribution& parallel_distribution) {
  std::string serialized_parallel_distribution;
  const int64_t num_axes = parallel_distribution.sbp_parallel_size();
  serialized_parallel_distribution += "[";
  for (int64_t i = 0; i < num_axes - 1; ++i) {
    serialized_parallel_distribution +=
        SbpParallelToString(parallel_distribution.sbp_parallel(i)) + " ";
  }
  serialized_parallel_distribution +=
      SbpParallelToString(parallel_distribution.sbp_parallel(num_axes - 1)) + "]";
  return serialized_parallel_distribution;
}

bool IsBreakpointOpNode(const OpNode* node) {
  // NOTE(chengcheng): breakpoint op is special which CANNOT through subgraph such as:
  //   variable, tick, repeat/acc/pack/unpack change timeshape
  const Operator& op = node->op();
  const OperatorConf& op_conf = op.op_conf();
  if (op_conf.has_variable_conf() || op_conf.has_tick_conf() || op_conf.has_device_tick_conf()
      || op_conf.has_src_subset_tick_conf() || op_conf.has_dst_subset_tick_conf()
      || op_conf.has_source_tick_conf() || op_conf.has_sink_tick_conf()
      || op_conf.has_acc_tick_conf()) {
    return true;
  }
  if (op_conf.has_user_conf()) {
    const std::string user_type_name = op_conf.user_conf().op_type_name();
    if (user_type_name == "repeat" || user_type_name == "acc" || user_type_name == "pack"
        || user_type_name == "unpack") {
      return true;
    }
  }
  return false;
}

bool IsAccOpNode(const OpNode* node) {
  return node->op().op_conf().has_user_conf()
         && node->op().op_conf().user_conf().op_type_name() == "acc";
}

const Shape GetOpNodeTimeShape(const OpNode* op_node) {
  return *(CHECK_JUST(op_node->op().GetOpTimeShape()).get());
}

const Shape GetOpNodeInputTimeShape(const OpNode* op_node) {
  return *(CHECK_JUST(op_node->op().GetInputBlobFastestTimeShape()).get());
}

void FindMaxConnectedSubgraphForGpuExecOrder(HashSet<const OpNode*>* ret, const OpGraph& op_graph,
                                             const std::vector<const OpNode*>& order) {
  HashSet<const OpNode*> visited;

  for (const OpNode* seed_node : order) {
    if (visited.find(seed_node) != visited.end()) { continue; }
    CHECK(visited.insert(seed_node).second);
    const ParallelDesc& seed_parallel_desc = seed_node->parallel_desc();
    // NOTE(chengcheng): ONLY consider GPU op and parallel num > 1.
    if (seed_parallel_desc.device_type() != DeviceType::kGPU) { continue; }
    if (seed_parallel_desc.parallel_num() <= 1) { continue; }
    if (IsBreakpointOpNode(seed_node)) { continue; }

    HashSet<const OpNode*> this_subgraph;
    std::queue<const OpNode*> queued_nodes;

    const Shape seed_time_shape = GetOpNodeTimeShape(seed_node);
    queued_nodes.push(seed_node);
    while (!queued_nodes.empty()) {
      const OpNode* cur_node = queued_nodes.front();
      queued_nodes.pop();

      CHECK(cur_node->parallel_desc().EqualsIgnoringHierarchy(seed_parallel_desc));
      CHECK(this_subgraph.insert(cur_node).second);

      cur_node->ForEachNodeOnInOutEdge([&](const OpNode* next_node) {
        if (visited.find(next_node) == visited.end()
            && next_node->parallel_desc().EqualsIgnoringHierarchy(seed_parallel_desc)
            && GetOpNodeTimeShape(next_node) == seed_time_shape) {
          CHECK(visited.insert(next_node).second);
          queued_nodes.push(next_node);
        }
      });
    }

    if (this_subgraph.size() > ret->size()) { ret->swap(this_subgraph); }
  }
}

bool ParallelDistributionAllSameSplitParallel(const ParallelDistribution& parallel_distribution) {
  CHECK_GT(parallel_distribution.sbp_parallel_size(), 0);
  const SbpParallel& first_sbp = parallel_distribution.sbp_parallel(0);
  if (!first_sbp.has_split_parallel()) { return false; }
  FOR_RANGE(int64_t, i, 1, parallel_distribution.sbp_parallel_size()) {
    if (parallel_distribution.sbp_parallel(i) != first_sbp) { return false; }
  }
  return true;
}

bool TryBuildNcclBy1DHierarchy(OperatorConf* ret, const SbpParallel& src_sbp,
                               const SbpParallel& dst_sbp, const std::string& lbn,
                               const int64_t scope_symbol_id, const BlobDesc& logical_blob_desc,
                               const int64_t parallel_num) {
  if (src_sbp.has_partial_sum_parallel() && dst_sbp.has_broadcast_parallel()) {
    // P->B : AllReduce
    *ret = user_op::UserOpConfWrapperBuilder(kNcclLogicalOpNamePrefix + "-P2B-" + NewUniqueId())
               .Op("_nccl_logical_all_reduce")
               .Input("in", lbn)
               .Output("out")
               .ScopeSymbolId(scope_symbol_id)
               .Build()
               .op_conf();
    return true;
  } else if ((logical_blob_desc.shape().At(0) % parallel_num == 0)
             && (src_sbp.has_partial_sum_parallel() && dst_sbp.has_split_parallel())
             && (dst_sbp.split_parallel().axis() == 0)) {
    // P->S(0) : ReduceScatter
    *ret = user_op::UserOpConfWrapperBuilder(kNcclLogicalOpNamePrefix + "-P2S-" + NewUniqueId())
               .Op("_nccl_logical_reduce_scatter")
               .Input("in", lbn)
               .Output("out")
               .ScopeSymbolId(scope_symbol_id)
               .Build()
               .op_conf();
    return true;
  } else if ((logical_blob_desc.shape().At(0) % parallel_num == 0)
             && (src_sbp.has_split_parallel() && dst_sbp.has_broadcast_parallel())
             && (src_sbp.split_parallel().axis() == 0)) {
    // S(0)->B : AllGather
    *ret = user_op::UserOpConfWrapperBuilder(kNcclLogicalOpNamePrefix + "-S2B-" + NewUniqueId())
               .Op("_nccl_logical_all_gather")
               .Input("in", lbn)
               .Output("out")
               .ScopeSymbolId(scope_symbol_id)
               .Build()
               .op_conf();
    return true;
  } else if ((src_sbp.has_split_parallel() && dst_sbp.has_split_parallel())
             && (src_sbp.split_parallel().axis() != dst_sbp.split_parallel().axis())
             && (logical_blob_desc.shape().At(src_sbp.split_parallel().axis()) % parallel_num == 0)
             && (logical_blob_desc.shape().At(dst_sbp.split_parallel().axis()) % parallel_num
                 == 0)) {
    // S(in)->S(out) : All2All
    *ret = user_op::UserOpConfWrapperBuilder(kNcclLogicalOpNamePrefix + "-S2S-" + NewUniqueId())
               .Op("_nccl_logical_s2s")
               .Input("in", lbn)
               .Output("out")
               .Attr<int64_t>("in_split_axis", src_sbp.split_parallel().axis())
               .Attr<int64_t>("out_split_axis", dst_sbp.split_parallel().axis())
               .ScopeSymbolId(scope_symbol_id)
               .Build()
               .op_conf();
    return true;
  }
  return false;
}

bool TryBuildNcclBy2DHierarchySameDim0(OperatorConf* ret,
                                       const ParallelDistribution& src_parallel_distribution,
                                       const ParallelDistribution& dst_parallel_distribution,
                                       const std::shared_ptr<Shape> hierarchy,
                                       const std::string& lbn, const int64_t scope_symbol_id,
                                       const BlobDesc& logical_blob_desc) {
  CHECK_EQ(src_parallel_distribution.sbp_parallel_size(), 2);
  CHECK_EQ(dst_parallel_distribution.sbp_parallel_size(), 2);
  CHECK(src_parallel_distribution.sbp_parallel(0) == dst_parallel_distribution.sbp_parallel(0));
  const SbpParallel& src_dim1_sbp = src_parallel_distribution.sbp_parallel(1);
  const SbpParallel& dst_dim1_sbp = dst_parallel_distribution.sbp_parallel(1);

  // split when dim0 sbp is split parallel
  DimVector dim_vec = logical_blob_desc.shape().dim_vec();
  if (src_parallel_distribution.sbp_parallel(0).has_split_parallel()) {
    const int64_t axis = src_parallel_distribution.sbp_parallel(0).split_parallel().axis();
    dim_vec.at(axis) /= hierarchy->At(0);
  }
  const int64_t num_ranks = hierarchy->At(1);

  if (src_dim1_sbp.has_partial_sum_parallel() && dst_dim1_sbp.has_broadcast_parallel()) {
    // (*, P)->(*, B) : AllReduce
    *ret =
        user_op::UserOpConfWrapperBuilder(kNcclLogicalOpNamePrefix + "-(*P)2(*B)-" + NewUniqueId())
            .Op("_nccl_logical_2D_same_dim0_all_reduce")
            .Input("in", lbn)
            .Output("out")
            .ScopeSymbolId(scope_symbol_id)
            .Build()
            .op_conf();
    return true;
  } else if ((dim_vec.at(0) % num_ranks == 0)
             && (src_dim1_sbp.has_split_parallel() && dst_dim1_sbp.has_broadcast_parallel())
             && (src_dim1_sbp.split_parallel().axis() == 0)) {
    // (*, S(0)) -> (*, B) : AllGather
    *ret =
        user_op::UserOpConfWrapperBuilder(kNcclLogicalOpNamePrefix + "-(*S0)2(*B)-" + NewUniqueId())
            .Op("_nccl_logical_2D_same_dim0_all_gather")
            .Input("in", lbn)
            .Output("out")
            .ScopeSymbolId(scope_symbol_id)
            .Build()
            .op_conf();
    return true;
  } else if (src_dim1_sbp.has_split_parallel() && dst_dim1_sbp.has_broadcast_parallel()
             && (src_dim1_sbp.split_parallel().axis() > 0)
             && (dim_vec.at(src_dim1_sbp.split_parallel().axis()) % num_ranks == 0)) {
    // (*, S(1)) -> (*, B) : AllGather Noncontinuous
    *ret =
        user_op::UserOpConfWrapperBuilder(kNcclLogicalOpNamePrefix + "-(*S1)2(*B)-" + NewUniqueId())
            .Op("_nccl_logical_2D_same_dim0_all_gather_noncontinuous")
            .Input("in", lbn)
            .Output("out")
            .Attr<int64_t>("in_dim1_split_axis", src_dim1_sbp.split_parallel().axis())
            .ScopeSymbolId(scope_symbol_id)
            .Build()
            .op_conf();
    return true;
  } else if ((src_dim1_sbp.has_split_parallel() && dst_dim1_sbp.has_split_parallel())
             && (src_dim1_sbp.split_parallel().axis() != dst_dim1_sbp.split_parallel().axis())
             && (dim_vec.at(src_dim1_sbp.split_parallel().axis()) % num_ranks == 0)
             && (dim_vec.at(dst_dim1_sbp.split_parallel().axis()) % num_ranks == 0)) {
    // (*, S(src_split_axis)) -> (*, S(dst_split_axis)) : All2All
    *ret =
        user_op::UserOpConfWrapperBuilder(kNcclLogicalOpNamePrefix + "-(*S)2(*S)-" + NewUniqueId())
            .Op("_nccl_logical_2D_same_dim0_all2all")
            .Input("in", lbn)
            .Output("out")
            .Attr<int64_t>("in_dim1_split_axis", src_dim1_sbp.split_parallel().axis())
            .Attr<int64_t>("out_dim1_split_axis", dst_dim1_sbp.split_parallel().axis())
            .ScopeSymbolId(scope_symbol_id)
            .Build()
            .op_conf();
    return true;
  }
  return false;
}

bool TryBuildNcclBy2DHierarchySameDim1(OperatorConf* ret,
                                       const ParallelDistribution& src_parallel_distribution,
                                       const ParallelDistribution& dst_parallel_distribution,
                                       const std::shared_ptr<Shape> hierarchy,
                                       const std::string& lbn, const int64_t scope_symbol_id,
                                       const BlobDesc& logical_blob_desc) {
  CHECK_EQ(src_parallel_distribution.sbp_parallel_size(), 2);
  CHECK_EQ(dst_parallel_distribution.sbp_parallel_size(), 2);
  CHECK(src_parallel_distribution.sbp_parallel(1) == dst_parallel_distribution.sbp_parallel(1));
  const SbpParallel& src_dim1_sbp = src_parallel_distribution.sbp_parallel(0);
  const SbpParallel& dst_dim1_sbp = dst_parallel_distribution.sbp_parallel(0);
  if (src_dim1_sbp.has_partial_sum_parallel() && dst_dim1_sbp.has_broadcast_parallel()) {
    // (P, *) -> (B, *) : AllReduce
    *ret =
        user_op::UserOpConfWrapperBuilder(kNcclLogicalOpNamePrefix + "-(P*)2(B*)-" + NewUniqueId())
            .Op("_nccl_logical_2D_same_dim1_all_reduce")
            .Input("in", lbn)
            .Output("out")
            .ScopeSymbolId(scope_symbol_id)
            .Build()
            .op_conf();
    return true;
  }
  return false;
}

bool TryBuildNcclLogicalOpConf(OperatorConf* ret, const OpNode* src_node, const OpNode* dst_node,
                               const LogicalBlobId& lbi) {
  if (!src_node->op().op_conf().has_scope_symbol_id()) { return false; /* device_tick */ }
  const int64_t scope_symbol_id = src_node->op().op_conf().scope_symbol_id();
  const std::string lbn = GenLogicalBlobName(lbi);
  const BlobDesc& logical_blob_desc = src_node->LogicalBlobDesc4Lbi(lbi);

  // reduce hierarchy
  ParallelDesc src_parallel_desc = src_node->parallel_desc();
  ParallelDesc dst_parallel_desc = dst_node->parallel_desc();
  ParallelDistribution src_parallel_distribution;
  ParallelDistribution dst_parallel_distribution;
  InOutParallelDimReduce(
      src_node->parallel_desc(), dst_node->parallel_desc(), src_node->ParallelDistribution4Lbi(lbi),
      dst_node->ParallelDistribution4Lbi(lbi), &src_parallel_desc, &dst_parallel_desc,
      &src_parallel_distribution, &dst_parallel_distribution);

  const int64_t parallel_num = src_parallel_desc.parallel_num();
  CHECK_EQ(parallel_num, dst_parallel_desc.parallel_num());
  const std::shared_ptr<Shape> src_hierarchy = src_parallel_desc.hierarchy();
  const std::shared_ptr<Shape> dst_hierarchy = dst_parallel_desc.hierarchy();

  if ((*src_hierarchy) == (*dst_hierarchy)
      && src_parallel_distribution == dst_parallel_distribution) {
    // one to one
    return false;
  }

  // NOTE(chengcheng): nccl donot support dynamic shape.
  if (logical_blob_desc.is_dynamic()) { return false; }
  CHECK_GT(logical_blob_desc.shape().elem_cnt(), 0);
  CHECK_GT(logical_blob_desc.shape().NumAxes(), 0);

  if (src_hierarchy->NumAxes() == 1 && dst_hierarchy->NumAxes() == 1) {
    return TryBuildNcclBy1DHierarchy(ret, src_parallel_distribution.sbp_parallel(0),
                                     dst_parallel_distribution.sbp_parallel(0), lbn,
                                     scope_symbol_id, logical_blob_desc, parallel_num);
  } else if (src_hierarchy->NumAxes() == 2 && (*src_hierarchy == *dst_hierarchy)) {
    if (src_parallel_distribution.sbp_parallel(0) == dst_parallel_distribution.sbp_parallel(0)) {
      return TryBuildNcclBy2DHierarchySameDim0(ret, src_parallel_distribution,
                                               dst_parallel_distribution, src_hierarchy, lbn,
                                               scope_symbol_id, logical_blob_desc);
    } else if (src_parallel_distribution.sbp_parallel(1)
               == dst_parallel_distribution.sbp_parallel(1)) {
      if (!(ParallelDistributionAllSameSplitParallel(src_parallel_distribution)
            || ParallelDistributionAllSameSplitParallel(dst_parallel_distribution))) {
        return TryBuildNcclBy2DHierarchySameDim1(ret, src_parallel_distribution,
                                                 dst_parallel_distribution, src_hierarchy, lbn,
                                                 scope_symbol_id, logical_blob_desc);
      }
    }
  }
  return false;
}

bool ReverseOrderInsertNcclLogicalOps() {
  return Global<ResourceDesc, ForSession>::Get()->resource().disable_group_boxing_by_dst_parallel();
}

void InsertNcclLogicalOpsAsCloseAsPossibleToSrcNode(
    HashMap<std::string, OperatorConf>* subgraph_op_name2conf, HashSet<std::string>* mut_op_names,
    std::vector<OperatorConf>* nccl_op_confs, std::vector<ParallelConf>* nccl_op_parallel_confs,
    const std::vector<const OpNode*>& subgraph_order,
    const HashMap<const OpNode*, int64_t>& node2order) {
  for (const OpNode* src_node : subgraph_order) {
    const std::string& src_op_name = src_node->op().op_name();
    for (const OpEdge* op_edge : src_node->out_edges()) {
      const OpNode* dst_node = op_edge->dst_node();
      const std::string& dst_op_name = dst_node->op().op_name();
      CHECK(src_node != dst_node);
      if (subgraph_op_name2conf->find(dst_op_name) == subgraph_op_name2conf->end()) {
        // NOTE(chengcheng): child node is not in this subgraph.
        continue;
      }
      for (const LogicalBlobId& lbi : op_edge->lbis()) {
        OperatorConf nccl_op;
        if (!TryBuildNcclLogicalOpConf(&nccl_op, src_node, dst_node, lbi)) { continue; }
        mut_op_names->insert(dst_op_name);
        // insert nccl op
        user_op::UserOpConfWrapper nccl_op_wrapper(nccl_op);
        for (const std::string& ibn : op_edge->lbi2ibns().at(lbi)) {
          std::string old_lbn = ReplaceInputLbnInOpCustomizedConf(
              &subgraph_op_name2conf->at(dst_op_name), ibn, nccl_op_wrapper.output("out", 0));
        }

        if (nccl_op_confs->size() >= 1) {
          // NOTE(chengcheng): MUST add ctrl edge between nccl ops for 1 src node insert multi-nccl
          const std::string& pre_nccl_op_name = nccl_op_confs->at(nccl_op_confs->size() - 1).name();
          nccl_op.add_ctrl_in_op_name(pre_nccl_op_name);
        }

        // NOTE(chengcheng): src_node MUST not the last node in subgraph, find the next op
        int64_t src_order = node2order.at(src_node);
        CHECK(src_order + 1 < subgraph_order.size());
        const std::string& next_op_name = subgraph_order.at(src_order + 1)->op().op_name();
        if (dst_op_name != next_op_name) {
          // NOTE(chengcheng): MUST add ctrl edge for strict exec order
          subgraph_op_name2conf->at(next_op_name).add_ctrl_in_op_name(nccl_op.name());
          mut_op_names->insert(next_op_name);
        }

        if (Global<ResourceDesc, ForSession>::Get()->enable_debug_mode()) {
          LOG(INFO) << "cc_debug_log: insert nccl op: " << nccl_op.name() << " from: ["
                    << src_op_name << "](order=" << src_order << ", sbp_parallel_dis="
                    << ParallelDistributionToString(src_node->ParallelDistribution4Lbi(lbi))
                    << ")->[" << dst_op_name << "](order=" << node2order.at(dst_node)
                    << ", sbp_parallel_dis="
                    << ParallelDistributionToString(dst_node->ParallelDistribution4Lbi(lbi))
                    << ") and before: [" << next_op_name << "](order=" << src_order + 1 << ")\n";
        }
        nccl_op_confs->push_back(nccl_op);
        nccl_op_parallel_confs->push_back(src_node->parallel_desc().parallel_conf());
      }
    }
  }
}

void InsertNcclLogicalOpsAsCloseAsPossibleToDstNode(
    HashMap<std::string, OperatorConf>* subgraph_op_name2conf, HashSet<std::string>* mut_op_names,
    std::vector<OperatorConf>* nccl_op_confs, std::vector<ParallelConf>* nccl_op_parallel_confs,
    const std::vector<const OpNode*>& subgraph_order,
    const HashMap<const OpNode*, int64_t>& node2order) {
  for (const OpNode* dst_node : subgraph_order) {
    const std::string& dst_op_name = dst_node->op().op_name();
    for (const OpEdge* op_edge : dst_node->in_edges()) {
      const OpNode* src_node = op_edge->src_node();
      const std::string& src_op_name = src_node->op().op_name();
      CHECK(src_node != dst_node);
      if (subgraph_op_name2conf->find(src_op_name) == subgraph_op_name2conf->end()) {
        // NOTE(chengcheng): parent node is not in this subgraph.
        continue;
      }
      for (const LogicalBlobId& lbi : op_edge->lbis()) {
        OperatorConf nccl_op;
        // builde nccl op
        if (!TryBuildNcclLogicalOpConf(&nccl_op, src_node, dst_node, lbi)) { continue; }
        mut_op_names->insert(dst_op_name);
        // insert nccl op
        user_op::UserOpConfWrapper nccl_op_wrapper(nccl_op);
        for (const std::string& ibn : op_edge->lbi2ibns().at(lbi)) {
          std::string old_lbn = ReplaceInputLbnInOpCustomizedConf(
              &subgraph_op_name2conf->at(dst_op_name), ibn, nccl_op_wrapper.output("out", 0));
          CHECK(old_lbn == GenLogicalBlobName(lbi));
        }

        // add necessary ctrl edge for strict order
        if (nccl_op_confs->size() >= 1) {
          // NOTE(chengcheng): MUST add ctrl edge between nccl ops for 1 dst node insert multi-nccl
          const std::string& pre_nccl_op_name = nccl_op_confs->at(nccl_op_confs->size() - 1).name();
          nccl_op.add_ctrl_in_op_name(pre_nccl_op_name);
        }

        // NOTE(chengcheng): dst_node MUST not the first node in subgraph, find the Immediately
        //   previous op of dst_node.
        int64_t dst_order = node2order.at(dst_node);
        CHECK_GT(dst_order, 0);
        const std::string& pre_op_name = subgraph_order.at(dst_order - 1)->op().op_name();
        if (src_op_name != pre_op_name) {
          // NOTE(chengcheng): MUST add ctrl edge for strict exec order
          nccl_op.add_ctrl_in_op_name(pre_op_name);
        }

        if (Global<ResourceDesc, ForSession>::Get()->enable_debug_mode()) {
          LOG(INFO) << "cc_debug_log: insert nccl op: " << nccl_op.name() << " from: ["
                    << src_op_name << "](" << node2order.at(src_node) << ")->[" << dst_op_name
                    << "](" << dst_order << ") and after: [" << pre_op_name << "](" << dst_order - 1
                    << ")\n";
        }
        nccl_op_confs->push_back(nccl_op);
        // NOTE(chengcheng, guoran): set nccl op as src_node parallel_conf (hierarchy) may check
        //   failed in complier.
        nccl_op_parallel_confs->push_back(src_node->parallel_desc().parallel_conf());
      }
    }
  }
}

bool IsOpEdgeAllowInsertNccl(const OpEdge* edge, const Shape& seed_time_shape) {
  const OpNode* src_node = edge->src_node();
  const OpNode* dst_node = edge->dst_node();
  const ParallelDesc& src_parallel_desc = src_node->parallel_desc();
  return src_parallel_desc.device_type() == DeviceType::kGPU && src_parallel_desc.parallel_num() > 1
         && src_parallel_desc.EqualsIgnoringHierarchy(dst_node->parallel_desc())
         && GetOpNodeTimeShape(src_node) == seed_time_shape
         && GetOpNodeTimeShape(dst_node) == seed_time_shape;
}

struct InsertedNcclInfo {
  OperatorConf nccl_op_conf;
  ParallelConf nccl_parallel_conf;
  int64_t order;
  std::string debug_str;
};

void InsertNcclLogicalOpsAfterAcc(const OpGraph& op_graph,
                                  const std::vector<const OpNode*>& ordered_op_nodes,
                                  const std::vector<const OpNode*>& ordered_acc_op_nodes,
                                  const OperatorConf& bw_sink_acc_tick_conf,
                                  HashMap<std::string, OperatorConf>* mut_consumer_name2op,
                                  std::vector<OperatorConf>* nccl_op_confs,
                                  std::vector<ParallelConf>* nccl_op_parallel_confs) {
  HashMap<const OpNode*, int64_t> op_node2global_order;
  for (int64_t i = 0; i < ordered_op_nodes.size(); ++i) {
    op_node2global_order.emplace(ordered_op_nodes.at(i), i);
  }

  HashSet<const OpEdge*> visited;
  const Shape seed_time_shape = GetOpNodeTimeShape(ordered_acc_op_nodes.front());
  const std::string& bw_sink_acc_tick_op_name = bw_sink_acc_tick_conf.name();
  std::vector<InsertedNcclInfo> unordered_nccl_op_infos;

  for (const OpNode* acc : ordered_acc_op_nodes) {
    std::queue<const OpEdge*> queued_edges;
    for (const OpEdge* op_edge : acc->out_edges()) {
      if (IsOpEdgeAllowInsertNccl(op_edge, seed_time_shape)) {
        queued_edges.push(op_edge);
        CHECK(visited.insert(op_edge).second);
      }
    }

    // bfs search each edge after acc allow insert nccl. try insert.
    while (!queued_edges.empty()) {
      const OpEdge* op_edge = queued_edges.front();
      queued_edges.pop();

      for (const LogicalBlobId& lbi : op_edge->lbis()) {
        OperatorConf nccl_op;
        if (!TryBuildNcclLogicalOpConf(&nccl_op, op_edge->src_node(), op_edge->dst_node(), lbi)) {
          continue;
        }
        const OpNode* src_node = op_edge->src_node();
        const OpNode* dst_node = op_edge->dst_node();
        const std::string& src_op_name = src_node->op().op_name();
        const std::string& dst_op_name = dst_node->op().op_name();
        if (mut_consumer_name2op->find(dst_op_name) == mut_consumer_name2op->end()) {
          mut_consumer_name2op->emplace(dst_op_name, dst_node->op().op_conf());
        }
        // insert nccl op
        user_op::UserOpConfWrapper nccl_op_wrapper(nccl_op);
        for (const std::string& ibn : op_edge->lbi2ibns().at(lbi)) {
          std::string old_lbn = ReplaceInputLbnInOpCustomizedConf(
              &mut_consumer_name2op->at(dst_op_name), ibn, nccl_op_wrapper.output("out", 0));
        }

        InsertedNcclInfo nccl_op_info;
        nccl_op_info.nccl_op_conf = nccl_op;
        nccl_op_info.nccl_parallel_conf = src_node->parallel_desc().parallel_conf();
        nccl_op_info.order = op_node2global_order.at(src_node);
        nccl_op_info.debug_str =
            ("cc_debug_log: After ACC insert nccl op: " + nccl_op.name() + " from: [" + src_op_name
             + "](" + ParallelDistributionToString(src_node->ParallelDistribution4Lbi(lbi)) + ")->["
             + dst_op_name + "]("
             + ParallelDistributionToString(dst_node->ParallelDistribution4Lbi(lbi))
             + "), src_order = " + std::to_string(nccl_op_info.order) + "\n");
        unordered_nccl_op_infos.push_back(nccl_op_info);
      }

      for (const OpEdge* dst_node_out_edge : op_edge->dst_node()->out_edges()) {
        if (visited.find(dst_node_out_edge) == visited.end()
            && IsOpEdgeAllowInsertNccl(dst_node_out_edge, seed_time_shape)) {
          CHECK(visited.insert(dst_node_out_edge).second);
          queued_edges.push(dst_node_out_edge);
        }
      }
    }
  }

  std::sort(unordered_nccl_op_infos.begin(), unordered_nccl_op_infos.end(),
            [](const InsertedNcclInfo& lhs, const InsertedNcclInfo& rhs) {
              return lhs.order < rhs.order;
            });

  for (int32_t i = 0; i < unordered_nccl_op_infos.size(); ++i) {
    auto& info = unordered_nccl_op_infos.at(i);
    if (i == 0) {
      info.nccl_op_conf.add_ctrl_in_op_name(bw_sink_acc_tick_op_name);
    } else {
      info.nccl_op_conf.add_ctrl_in_op_name(unordered_nccl_op_infos.at(i - 1).nccl_op_conf.name());
    }
    nccl_op_confs->push_back(info.nccl_op_conf);
    nccl_op_parallel_confs->push_back(info.nccl_parallel_conf);
    LOG(INFO) << info.debug_str;
  }
}

Maybe<void> InsertNcclLogicalOpPass::Apply(const OpGraph& op_graph, JobBuilder* job_builder) const {
  auto OpGraphForEachInDataAndCtrlNode = [&](OpNode* node,
                                             const std::function<void(OpNode*)>& Handler) {
    op_graph.ForEachDataAndCtrlInNode(node, Handler);
  };
  auto OpGraphForEachOutDataAndCtrlNode = [&](OpNode* node,
                                              const std::function<void(OpNode*)>& Handler) {
    op_graph.ForEachDataAndCtrlOutNode(node, Handler);
  };

  std::vector<const OpNode*> ordered_op_nodes;
  op_graph.TopoForEachNode(op_graph.DataOrCtrlSourceNodes(), OpGraphForEachInDataAndCtrlNode,
                           OpGraphForEachOutDataAndCtrlNode,
                           [&](const OpNode* node) { ordered_op_nodes.push_back(node); });

  HashSet<const OpNode*> subgraph;
  FindMaxConnectedSubgraphForGpuExecOrder(&subgraph, op_graph, ordered_op_nodes);
  if (subgraph.size() <= 1) { return Maybe<void>::Ok(); }

  std::vector<const OpNode*> subgraph_order;
  HashMap<const OpNode*, int64_t> node2order;
  std::vector<const OpNode*> ordered_acc_op_nodes;
  for (const OpNode* this_node : ordered_op_nodes) {
    if (subgraph.find(this_node) != subgraph.end()) {
      subgraph_order.push_back(this_node);
      node2order.emplace(this_node, subgraph_order.size() - 1);
    } else if (IsAccOpNode(this_node)) {
      ordered_acc_op_nodes.push_back(this_node);
    }
  }

  CHECK_EQ(subgraph.size(), subgraph_order.size());

  HashSet<std::string> mut_op_names;
  const OpNode* first_node = subgraph_order.at(0);
  HashMap<std::string, OperatorConf> subgraph_op_name2conf;
  subgraph_op_name2conf.emplace(first_node->op().op_name(), first_node->op().op_conf());
  auto IsReachable = op_graph.MakePredicatorIsOpNameDataOrCtrlReachable();
  for (int32_t i = 1; i < subgraph_order.size(); ++i) {
    const OpNode* this_node = subgraph_order.at(i);
    const OpNode* pre_node = subgraph_order.at(i - 1);
    const std::string& this_op_name = this_node->op().op_name();
    const std::string& pre_op_name = pre_node->op().op_name();
    CHECK(subgraph_op_name2conf.emplace(this_op_name, this_node->op().op_conf()).second);
    // build control edge if need.
    if (!IsReachable(pre_op_name, this_op_name)) {
      subgraph_op_name2conf.at(this_op_name).add_ctrl_in_op_name(pre_op_name);
      mut_op_names.insert(this_op_name);
    }
  }

  if (Global<ResourceDesc, ForSession>::Get()->enable_debug_mode()) {
    LOG(INFO) << "cc_debug_log: Try insert nccl logical ops into job: "
              << job_builder->job().job_conf().job_name() << ". Begin...\n";
  }

  std::vector<OperatorConf> nccl_op_confs;
  std::vector<ParallelConf> nccl_op_parallel_confs;
  if (ReverseOrderInsertNcclLogicalOps()) {
    InsertNcclLogicalOpsAsCloseAsPossibleToDstNode(&subgraph_op_name2conf, &mut_op_names,
                                                   &nccl_op_confs, &nccl_op_parallel_confs,
                                                   subgraph_order, node2order);
  } else {
    InsertNcclLogicalOpsAsCloseAsPossibleToSrcNode(&subgraph_op_name2conf, &mut_op_names,
                                                   &nccl_op_confs, &nccl_op_parallel_confs,
                                                   subgraph_order, node2order);
  }

  if (!ordered_acc_op_nodes.empty()
      && Global<ResourceDesc, ForSession>::Get()->enable_debug_mode()) {
    LOG(WARNING)
        << "cc_debug_log: Find acc op in Job: " << job_builder->job().job_conf().job_name()
        << ", we will try insert special identity and ctrl for "
        << " UNSAFE handle ALL nccl ops between different time shape (n,) -> acc -> (1, )\n\n";
    const OpNode* bw_sink_op = subgraph_order.back();
    const OpNode* first_acc_op = ordered_acc_op_nodes.front();
    const Shape time_shape_before_acc = GetOpNodeTimeShape(bw_sink_op);
    const Shape time_shape_after_acc = GetOpNodeTimeShape(first_acc_op);
    LOG(INFO) << "cc_debug_log: acc time shape from: " << time_shape_before_acc.DebugStr()
              << " to: " << time_shape_after_acc.DebugStr();
    CHECK_GT(time_shape_before_acc.elem_cnt(), time_shape_after_acc.elem_cnt());
    CHECK_EQ(time_shape_before_acc.elem_cnt() % time_shape_after_acc.elem_cnt(), 0);

    for (const OpNode* acc : ordered_acc_op_nodes) {
      CHECK_EQ(time_shape_before_acc, GetOpNodeInputTimeShape(acc));
      CHECK_EQ(time_shape_after_acc, GetOpNodeTimeShape(acc));
    }

    // NOTE(chengcheng): insert acc_tick after bw_sink_op, and this tick op conf will control
    //  after_acc_nccl_ops start.
    const auto& obns = bw_sink_op->op().output_bns();
    CHECK(!obns.empty());
    const std::string bw_sink_op_out_lbn =
        GenLogicalBlobName(bw_sink_op->op().BnInOp2Lbi(obns.Get(0)));

    LOG(INFO) << "cc_debug_log: bw_sink_op : " << bw_sink_op->op().op_conf().DebugString();
    user_op::UserOpConfWrapper cast_to_tick_op =
        user_op::UserOpConfWrapperBuilder("System-CastToTick-" + NewUniqueId())
            .OpTypeName("cast_to_tick")
            .Input("in", bw_sink_op_out_lbn)
            .Output("out")
            .Build();
    job_builder->AddOp(bw_sink_op->parallel_desc().parallel_conf(), cast_to_tick_op.op_conf());
    LOG(INFO) << "cc_debug_log: cast_to_tick_op : " << cast_to_tick_op.op_conf().DebugString();

    OperatorConf bw_sink_acc_tick_conf;
    bw_sink_acc_tick_conf.set_name(std::string("System-BwSinkTick-AccTick_") + NewUniqueId());
    auto* acc_conf = bw_sink_acc_tick_conf.mutable_acc_tick_conf();
    acc_conf->set_one(cast_to_tick_op.output("out", 0));
    acc_conf->set_acc("acc");
    acc_conf->set_max_acc_num(time_shape_before_acc.elem_cnt() / time_shape_after_acc.elem_cnt());
    job_builder->AddOp(bw_sink_op->parallel_desc().parallel_conf(), bw_sink_acc_tick_conf);
    LOG(INFO) << "cc_debug_log: bw_sink_acc_tick_op : " << bw_sink_acc_tick_conf.DebugString();

    // insert nccl ops after acc
    std::vector<OperatorConf> after_acc_nccl_op_confs;
    std::vector<ParallelConf> after_acc_nccl_parallel_confs;
    std::vector<OperatorConf> after_acc_identity_op_confs;
    std::vector<ParallelConf> after_acc_identity_parallel_confs;
    HashMap<std::string, OperatorConf> mut_consumer_name2op;

    InsertNcclLogicalOpsAfterAcc(op_graph, ordered_op_nodes, ordered_acc_op_nodes,
                                 bw_sink_acc_tick_conf, &mut_consumer_name2op,
                                 &after_acc_nccl_op_confs, &after_acc_nccl_parallel_confs);

    for (const auto& pair : mut_consumer_name2op) {
      LOG(INFO) << "cc_debug_log: after acc mut consumer op: " << pair.second.DebugString();
      JUST(job_builder->MutOpOnlyOnce(pair.second));
    }
    CHECK_EQ(after_acc_nccl_op_confs.size(), after_acc_nccl_parallel_confs.size());
    CHECK_EQ(after_acc_identity_op_confs.size(), after_acc_identity_parallel_confs.size());
    for (int64_t i = 0; i < after_acc_nccl_op_confs.size(); ++i) {
      LOG(INFO) << "cc_debug_log: after acc insert nccl op: "
                << after_acc_nccl_op_confs.at(i).DebugString();
      job_builder->AddOp(after_acc_nccl_parallel_confs.at(i), after_acc_nccl_op_confs.at(i));
    }
    for (int64_t i = 0; i < after_acc_identity_op_confs.size(); ++i) {
      LOG(INFO) << "cc_debug_log: after acc insert identity op: "
                << after_acc_identity_op_confs.at(i).DebugString();
      job_builder->AddOp(after_acc_identity_parallel_confs.at(i),
                         after_acc_identity_op_confs.at(i));
    }
  }

  if (Global<ResourceDesc, ForSession>::Get()->enable_debug_mode()) {
    LOG(INFO) << "cc_debug_log: Try insert nccl logical ops into job: "
              << job_builder->job().job_conf().job_name() << ". ...End\n\n";
  }

  std::vector<OperatorConf> mut_op_confs;
  for (const std::string& mut_op_name : mut_op_names) {
    mut_op_confs.push_back(subgraph_op_name2conf.at(mut_op_name));
  }
  job_builder->MutOpsOnlyOnce(mut_op_confs);

  CHECK_EQ(nccl_op_confs.size(), nccl_op_parallel_confs.size());
  for (int64_t i = 0; i < nccl_op_confs.size(); ++i) {
    job_builder->AddOp(nccl_op_parallel_confs.at(i), nccl_op_confs.at(i));
  }

  return Maybe<void>::Ok();
}

}  // namespace

REGISTER_JOB_PASS("InsertNcclLogicalOpPass", InsertNcclLogicalOpPass);

}  // namespace oneflow
