#include "rpc.h"

namespace erpc {

template <class TTr>
void Rpc<TTr>::fake_process_resp(SSlot *sslot){
  if (!sslot->client_info_.cont_func_) return;

  auto &ci = sslot->client_info_;
  bump_credits(sslot->session_);
  
  assert(ci.wheel_count_ == 0);

  sslot->tx_msgbuf_ = nullptr;  // Mark response as received
  delete_from_active_rpc_list(*sslot);

  const erpc_cont_func_t cont_func = ci.cont_func_;
  void *tag = ci.tag_;
  const size_t cont_etid = ci.cont_etid_;

  Session *session = sslot->session_;
  session->client_info_.sslot_free_vec_.push_back(sslot->index_);

  // Clear up one request from the backlog if needed
  if (!session->client_info_.enq_req_backlog_.empty()) {
    // We just got a new sslot, and we should have no more if there's backlog
    assert(session->client_info_.sslot_free_vec_.size() == 1);
    enq_req_args_t &args = session->client_info_.enq_req_backlog_.front();
    enqueue_request(args.session_num_, args.req_type_, args.req_msgbuf_,
                    args.resp_msgbuf_, args.cont_func_, args.tag_,
                    args.cont_etid_);
    session->client_info_.enq_req_backlog_.pop();
  }

  if (likely(cont_etid == kInvalidBgETid)) {
    cont_func(context_, tag);
  } else {
    submit_bg_resp_st(cont_func, tag, cont_etid);
  }
  return;
}

template <class TTr>
void Rpc<TTr>::run_event_loop_do_one_st() {
  assert(in_dispatch());
  dpath_stat_inc(dpath_stats_.ev_loop_calls_, 1);

  // Handle any new session management packets
  if (unlikely(nexus_hook_.sm_rx_queue_.size_ > 0)) handle_sm_rx_st();

  // The packet RX code uses ev_loop_tsc as the RX timestamp, so it must be
  // next to ev_loop_tsc stamping.
  ev_loop_tsc_ = dpath_rdtsc();
  process_comps_st();  // RX

  process_credit_stall_queue_st();    // TX
  if (kCcPacing) process_wheel_st();  // TX

  // Drain all packets
  if (tx_batch_i_ > 0) do_tx_burst_st();

  if (unlikely(multi_threaded_)) {
    // Process the background queues
    process_bg_queues_enqueue_request_st();
    process_bg_queues_enqueue_response_st();
  }

  #ifdef KeepSend
  if(session_vec_.size() > 0){
    Session *session = session_vec_[0];
    if(session->is_client()){
      for(SSlot &sslot : session->sslot_arr_){
        fake_process_resp(&sslot);
      }
    }
  }
  #endif

  #ifndef KeepSend
  // Check for packet loss if we're in a new epoch. ev_loop_tsc is stale by
  // less than one event loop iteration, which is negligible compared to epoch.
  if (unlikely(ev_loop_tsc_ - pkt_loss_scan_tsc_ > rpc_pkt_loss_scan_cycles_)) {
    pkt_loss_scan_tsc_ = ev_loop_tsc_;
    pkt_loss_scan_st();
  }
  #endif
}

template <class TTr>
void Rpc<TTr>::run_event_loop_timeout_st(size_t timeout_ms) {
  assert(in_dispatch());

  size_t timeout_tsc = ms_to_cycles(timeout_ms, freq_ghz_);
  size_t start_tsc = rdtsc();  // For counting timeout_ms

  while (true) {
    run_event_loop_do_one_st();  // Run at least once even if timeout_ms is 0
    if (unlikely(ev_loop_tsc_ - start_tsc > timeout_tsc)) break;
  }
}

FORCE_COMPILE_TRANSPORTS

}  // namespace erpc
