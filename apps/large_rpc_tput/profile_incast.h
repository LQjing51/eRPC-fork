#ifndef PROFILE_INCAST_H
#define PROFILE_INCAST_H

#include "large_rpc_tput.h"

size_t get_session_idx_func_incast(AppContext *) {
  erpc::rt_assert(FLAGS_process_id != 0, "Process 0 cannot send reqs.");
  return 0;
}

void connect_sessions_func_incast(AppContext *c) {
  // All non-zero processes create one session to process #0
  if (FLAGS_process_id == 0) return;

  size_t global_thread_id =
      FLAGS_process_id * FLAGS_num_proc_other_threads + c->thread_id_;
  size_t rem_tid = global_thread_id % FLAGS_num_proc_0_threads;

  c->session_num_vec_.resize(1);

  printf(
      "large_rpc_tput: Thread %zu: Creating 1 session to proc 0, thread %zu.\n",
      c->thread_id_, rem_tid);

  c->session_num_vec_[0] =
      c->rpc_->create_session(erpc::get_uri_for_process(0), rem_tid);
  erpc::rt_assert(c->session_num_vec_[0] >= 0, "create_session() failed");

  while (c->num_sm_resps_ != 1) {
    if (unlikely(c->rpc_->nexus_hook_.sm_rx_queue_.size_ > 0)) c->rpc_->handle_sm_rx_st();
    // c->rpc_->run_event_loop(200);  // 200 milliseconds
    if (ctrl_c_pressed == 1) return;
  }
  erpc::IBTransport::ib_routing_info_t* rinfo = c->rpc_->session_vec_[0]->is_client() ? reinterpret_cast<erpc::IBTransport::ib_routing_info_t*>(&c->rpc_->session_vec_[0]->server_.routing_info_) : reinterpret_cast<erpc::IBTransport::ib_routing_info_t*>(&c->rpc_->session_vec_[0]->client_.routing_info_);
  c->rpc_->transport_->init_verbs_structs(rinfo);
  if (c->rpc_->session_vec_[0]->is_client()) {
    c->rpc_->transport_->fill_local_routing_info(&c->rpc_->session_vec_[0]->client_.routing_info_);
  } else {
    c->rpc_->transport_->fill_local_routing_info(&c->rpc_->session_vec_[0]->server_.routing_info_);
  }
  // Complete transport initialization using the hugepage allocator
  c->rpc_->transport_->init_hugepage_structures(c->rpc_->huge_alloc_, c->rpc_->rx_ring_);

  if (FLAGS_throttle == 1) {
    erpc::Timely *timely_0 = c->rpc_->get_timely(c->session_num_vec_[0]);
    double num_flows = (FLAGS_num_processes - 1) * FLAGS_num_proc_other_threads;
    double fair_share = c->rpc_->get_bandwidth() / num_flows;

    timely_0->rate_ = fair_share * FLAGS_throttle_fraction;
  }
}

#endif
