/**
 * @file large_rpc_tput.cc
 *
 * @brief Benchmark to measure large RPC throughput. Each thread measures its
 * RX and TX bandwidth.
 *
 * Each thread creates at most one session. Session connectivity is controlled
 * by the "profile" flag. The available profiles are:
 *   o incast: Incast
 *   o victim: With N processes {0, ..., N - 1}, where N >= 3:
 *     o Process 0 and process (N - 1) do not send requests
 *     o All threads on processes 1 through (N - 2) incast to process 0
 *     o Thread T - 1 on processes (N - 2) sends requests to process (N - 1)
 */

#include "large_rpc_tput.h"
#include <signal.h>
#include <cstring>
#include <mutex>
#include <fstream>
#include "profile_incast.h"
#include "profile_victim.h"
#include "util/autorun_helpers.h"

static constexpr size_t kAppEvLoopMs = 1000;  // Duration of event loop
static constexpr bool kAppVerbose = false;

// Global data structure for collecting latency samples from all threads
static std::mutex lat_export_mutex;
static std::vector<double> global_lat_vec;

// Experiment control flags
static constexpr bool kAppClientMemsetReq = false;   // Fill entire request
static constexpr bool kAppServerMemsetResp = true;  // Fill entire response
static constexpr bool kAppClientCheckResp = false;   // Check entire response
static constexpr bool kAppServerCheckReq = true;   // Check entire request

// Profile-specifc session connection function
std::function<void(AppContext *)> connect_sessions_func = nullptr;

void app_cont_func(void *, void *);  // Forward declaration

// Send a request using this MsgBuffer
void send_req(AppContext *c, size_t msgbuf_idx, size_t req_size) {
  erpc::MsgBuffer &req_msgbuf = c->req_msgbuf[msgbuf_idx];
  assert(req_msgbuf.get_data_size() == req_size);

  if (kAppVerbose) {
    printf("large_rpc_tput: Thread %zu sending request using msgbuf_idx %zu.\n",
           c->thread_id_, msgbuf_idx);
  }

  c->req_ts[msgbuf_idx] = erpc::rdtsc();
  int ret = c->rpc_->enqueue_request(c->session_num_vec_[0], kAppReqType, &req_msgbuf,
                           &c->resp_msgbuf[msgbuf_idx], app_cont_func,
                           reinterpret_cast<void *>(msgbuf_idx));
  if(ret) c->stat_tx_bytes_tot += req_size;
}

  void add_ticks() {
    uint64_t s_tick, passed_ticks = 0;
    s_tick = erpc::rdtsc();
    do {
      passed_ticks = erpc::rdtsc() - s_tick;
    } while(passed_ticks < FLAGS_app_ticks);
  }

void req_handler(erpc::ReqHandle *req_handle, void *_context) {
  auto *c = static_cast<AppContext *>(_context);
  const erpc::MsgBuffer *req_msgbuf = req_handle->get_req_msgbuf();
  uint8_t resp_byte = req_msgbuf->buf_[0];

  size_t req_size = req_msgbuf->get_data_size();
  if (kAppServerCheckReq) {
    bool match = true;
    // Check all request cachelines (checking every byte is slow)
    for (size_t i = 0; i < req_size; i += 64) {
      if (req_msgbuf->buf_[i] != kAppDataByte) match = false;
    }
    erpc::rt_assert(match, "Invalid req data");
  } else {
    erpc::rt_assert(req_msgbuf->buf_[0] == kAppDataByte, "Invalid req data");
  }

  // Use dynamic response
  size_t resp_size = c->resp_msgbuf[0].get_data_size();

  erpc::MsgBuffer &resp_msgbuf = req_handle->dyn_resp_msgbuf_;
  resp_msgbuf = c->rpc_->alloc_msg_buffer_or_die(resp_size);

  // Touch the response
  if (kAppServerMemsetResp) {
    memset(resp_msgbuf.buf_, resp_byte, resp_size);
  } else {
    resp_msgbuf.buf_[0] = resp_byte;
  }

  c->stat_rx_bytes_tot += req_msgbuf->get_data_size();
  c->stat_tx_bytes_tot += resp_size;

  add_ticks();

  c->rpc_->enqueue_response(req_handle, &resp_msgbuf);
}

void app_cont_func(void *_context, void *_tag) {
  auto *c = static_cast<AppContext *>(_context);
  auto msgbuf_idx = reinterpret_cast<size_t>(_tag);
  const erpc::MsgBuffer &resp_msgbuf = c->resp_msgbuf[msgbuf_idx];
  if (kAppVerbose) {
    printf("large_rpc_tput: Received response for msgbuf %zu.\n", msgbuf_idx);
  }

  // Measure latency. 1 us granularity is sufficient for large RPC latency.
  double usec = erpc::to_usec(erpc::rdtsc() - c->req_ts[msgbuf_idx],
                              c->rpc_->get_freq_ghz());
  c->lat_vec.push_back(usec);

  // Check the response
  size_t resp_size = resp_msgbuf.get_data_size();

  if (kAppClientCheckResp) {
    bool match = true;
    // Check all response cachelines (checking every byte is slow)
    for (size_t i = 0; i < resp_size; i += 64) {
      if (resp_msgbuf.buf_[i] != kAppDataByte) match = false;
    }
    erpc::rt_assert(match, "Invalid resp data");
  } else {
    erpc::rt_assert(resp_msgbuf.buf_[0] == kAppDataByte, "Invalid resp data");
  }

  c->stat_rx_bytes_tot += resp_size;

  size_t buf_idx;
  buf_idx = msgbuf_idx;

  size_t req_size = c->req_msgbuf[buf_idx].get_data_size();
  if (kAppClientMemsetReq) {
    memset(c->req_msgbuf[buf_idx].buf_, kAppDataByte, req_size);
  } else {
    c->req_msgbuf[buf_idx].buf_[0] = kAppDataByte;
  }
  send_req(c, buf_idx, req_size);
}

// The function executed by each thread in the cluster
void thread_func(size_t thread_id, app_stats_t *app_stats, erpc::Nexus *nexus) {
  AppContext c;
  c.thread_id_ = thread_id;
  c.app_stats = app_stats;
  if (thread_id == 0)
    c.tmp_stat_ = new TmpStat(app_stats_t::get_template_str());

  std::vector<size_t> req_size_vec = flags_get_req_sizes();
  std::vector<size_t> resp_size_vec = flags_get_resp_sizes();
  std::vector<size_t> concur_vec = flags_get_concur();
  std::vector<size_t> port_vec = flags_get_numa_ports(FLAGS_numa_node);

  size_t req_size = req_size_vec.at(thread_id % req_size_vec.size());
  size_t resp_size = resp_size_vec.at(thread_id % resp_size_vec.size());
  size_t concur = concur_vec.at(thread_id % resp_size_vec.size());
  uint8_t phy_port = port_vec.at(thread_id % port_vec.size());

  erpc::rt_assert(concur <= kAppMaxConcurrency, "Invalid conc");

  erpc::Rpc<erpc::CTransport> rpc(nexus, static_cast<void *>(&c),
                                  static_cast<uint8_t>(thread_id),
                                  basic_sm_handler, phy_port);
  rpc.retry_connect_on_invalid_rpc_id_ = true;
  if (erpc::kTesting) rpc.fault_inject_set_pkt_drop_prob_st(FLAGS_drop_prob);

  c.rpc_ = &rpc;

  printf("freq: %.3f\n", rpc.get_freq_ghz());
  // Create the session. Some threads may not create any sessions, and therefore
  // not run the event loop required for other threads to connect them. This
  // is OK because all threads will run the event loop below.
  connect_sessions_func(&c);

  if (c.session_num_vec_.size() > 0) {
    printf("large_rpc_tput: Thread %zu: All sessions connected.\n", thread_id);
  } else {
    printf("large_rpc_tput: Thread %zu: No sessions created.\n", thread_id);
  }

  // All threads allocate MsgBuffers, but they may not send requests
  alloc_req_resp_msg_buffers(&c, req_size, resp_size, concur);

  size_t console_ref_tsc = erpc::rdtsc();

  // Any thread that creates a session sends requests
  if (c.session_num_vec_.size() > 0) {
    for (size_t msgbuf_idx = 0; msgbuf_idx < concur; msgbuf_idx++) {
      send_req(&c, msgbuf_idx, req_size);
    }
  }

  c.tput_t0.reset();
  for (size_t i = 0; i < FLAGS_test_ms; i += kAppEvLoopMs) {
    rpc.run_event_loop(kAppEvLoopMs);

    if (unlikely(ctrl_c_pressed == 1)) {
      if (erpc::HOSTCC && !erpc::client) {
        RhyR::hostcc_exit();
      }
      break;
    }
    // server print log
    if (erpc::HOSTCC && erpc::client){
      printf("Thread %zu:", c.thread_id_);
      RhyR::hostcc_print_stats();
    }
    if (erpc::SWIFT && erpc::client){
      printf("Thread %zu:", c.thread_id_);
      RhyR::swift_print_stats();
    }
    if (erpc::queue_size) {
      printf("Thread %zu: avg_poll_num %.2f\n", c.thread_id_, c.rpc_->avg_poll_num);
      c.rpc_->stats_count = 0;
      c.rpc_->avg_poll_num = 0;
    }

    if (c.session_num_vec_.size() == 0) continue;  // No stats to print

    const double ns = c.tput_t0.get_ns();
    erpc::Timely *timely_0 = c.rpc_->get_timely(0);

    // Publish stats
    auto &stats = c.app_stats[c.thread_id_];
    stats.rx_gbps = c.stat_rx_bytes_tot * 8 / ns;
    stats.tx_gbps = c.stat_tx_bytes_tot * 8 / ns;
    stats.re_tx = c.rpc_->get_num_re_tx(c.session_num_vec_[0]);
    stats.rtt_50_us = timely_0->get_rtt_perc(0.50);
    stats.rtt_99_us = timely_0->get_rtt_perc(0.99);

    if (c.lat_vec.size() > 0) {
      std::sort(c.lat_vec.begin(), c.lat_vec.end());
      stats.rpc_50_us = c.lat_vec[c.lat_vec.size() * 0.50];
      stats.rpc_99_us = c.lat_vec[c.lat_vec.size() * 0.99];
      stats.rpc_999_us = c.lat_vec[c.lat_vec.size() * 0.999];
    } else {
      // Even if no RPCs completed, we need retransmission counter
      stats.rpc_50_us = kAppEvLoopMs * 1000;
      stats.rpc_99_us = kAppEvLoopMs * 1000;
      stats.rpc_999_us = kAppEvLoopMs * 1000;
    }

    printf(
        "large_rpc_tput: Thread %zu: Tput {RX %.2f (%zu), TX %.2f (%zu)} "
        "Gbps (IOPS). Retransmissions %zu. Packet RTTs: {%.1f, %.1f} us. "
        "RPC latency {%.1f 50th, %.1f 99th, %.1f 99.9th}. Timely rate %.1f "
        "Gbps. Credits %zu (best = 32).\n",
        c.thread_id_, stats.rx_gbps*(resp_size+126)/resp_size, c.stat_rx_bytes_tot / resp_size,
        stats.rx_gbps*(req_size+126)/resp_size, c.stat_rx_bytes_tot / resp_size, stats.re_tx,
        stats.rtt_50_us, stats.rtt_99_us, stats.rpc_50_us, stats.rpc_99_us,
        stats.rpc_999_us, timely_0->get_rate_gbps(), c.rpc_->get_credits(c.session_num_vec_[0]));

    // Accumulate latency samples for export (before clearing)
    if (!FLAGS_latency_output_file.empty() && c.lat_vec.size() > 0) {
      c.lat_vec_accumulated.insert(c.lat_vec_accumulated.end(),
                                    c.lat_vec.begin(), c.lat_vec.end());
      // Also add to global vector (thread-safe)
      std::lock_guard<std::mutex> lock(lat_export_mutex);
      global_lat_vec.insert(global_lat_vec.end(),
                            c.lat_vec.begin(), c.lat_vec.end());
    }

    // Reset stats for next iteration
    c.stat_rx_bytes_tot = 0;
    c.stat_tx_bytes_tot = 0;
    c.rpc_->reset_num_re_tx(c.session_num_vec_[0]);
    c.lat_vec.clear();
    timely_0->reset_rtt_stats();

    if (c.thread_id_ == 0) {
      app_stats_t accum_stats;
      for (size_t i = 0; i < FLAGS_num_proc_other_threads; i++) {
        accum_stats += c.app_stats[i];
      }

      // Compute averages for non-additive stats
      accum_stats.rtt_50_us /= FLAGS_num_proc_other_threads;
      accum_stats.rtt_99_us /= FLAGS_num_proc_other_threads;
      accum_stats.rpc_50_us /= FLAGS_num_proc_other_threads;
      accum_stats.rpc_99_us /= FLAGS_num_proc_other_threads;
      accum_stats.rpc_999_us /= FLAGS_num_proc_other_threads;
      c.tmp_stat_->write(accum_stats.to_string());
    }

    c.tput_t0.reset();
  }

  erpc::TimingWheel *wheel = rpc.get_wheel();
  if (wheel != nullptr && !wheel->record_vec_.empty()) {
    const size_t num_to_print = 200;
    const size_t tot_entries = wheel->record_vec_.size();
    const size_t base_entry = tot_entries * .9;

    printf("Printing up to 200 entries toward the end of wheel record\n");
    size_t num_printed = 0;

    for (size_t i = base_entry; i < tot_entries; i++) {
      auto &rec = wheel->record_vec_.at(i);
      printf("wheel: %s\n",
             rec.to_string(console_ref_tsc, rpc.get_freq_ghz()).c_str());

      if (num_printed++ == num_to_print) break;
    }
  }

  // Accumulate any remaining latency samples
  if (!FLAGS_latency_output_file.empty() && c.lat_vec.size() > 0) {
    c.lat_vec_accumulated.insert(c.lat_vec_accumulated.end(),
                                  c.lat_vec.begin(), c.lat_vec.end());
    // Also add to global vector (thread-safe)
    std::lock_guard<std::mutex> lock(lat_export_mutex);
    global_lat_vec.insert(global_lat_vec.end(),
                          c.lat_vec.begin(), c.lat_vec.end());
  }

  // We don't disconnect sessions
}

// Use the supplied profile set up globals and possibly modify other flags
void setup_profile() {
  if (FLAGS_profile == "incast") {
    connect_sessions_func = connect_sessions_func_incast;
    return;
  }

  if (FLAGS_profile == "victim") {
    erpc::rt_assert(FLAGS_num_processes >= 3, "Too few processes");
    erpc::rt_assert(FLAGS_num_proc_other_threads >= 2, "Too few threads");
    connect_sessions_func = connect_sessions_func_victim;
    return;
  }
}

int main(int argc, char **argv) {
  signal(SIGINT, ctrl_c_handler);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  erpc::rt_assert(FLAGS_profile == "incast" || FLAGS_profile == "victim",
                  "Invalid profile");
  erpc::rt_assert(FLAGS_process_id < FLAGS_num_processes, "Invalid process ID");

  if (!erpc::kTesting) {
    erpc::rt_assert(FLAGS_drop_prob == 0.0, "Invalid drop prob");
  } else {
    erpc::rt_assert(FLAGS_drop_prob < 1, "Invalid drop prob");
  }

  setup_profile();
  erpc::rt_assert(connect_sessions_func != nullptr, "No connect_sessions_func");

  erpc::Nexus nexus(erpc::get_uri_for_process(FLAGS_process_id),
                    FLAGS_numa_node, 0);
  nexus.register_req_func(kAppReqType, req_handler);

  size_t num_threads = FLAGS_process_id == 0 ? FLAGS_num_proc_0_threads
                                             : FLAGS_num_proc_other_threads;
  std::vector<std::thread> threads(num_threads);
  auto *app_stats = new app_stats_t[num_threads];

  for (size_t i = 0; i < num_threads; i++) {
    threads[i] = std::thread(thread_func, i, app_stats, &nexus);
    erpc::bind_to_core(threads[i], FLAGS_numa_node, i);
  }

  for (auto &thread : threads) thread.join();

  // Export latency data to file if requested
  if (!FLAGS_latency_output_file.empty()) {
    std::lock_guard<std::mutex> lock(lat_export_mutex);

    const size_t target_samples = 200;
    const size_t total_samples = global_lat_vec.size();

    if (total_samples >= target_samples) {
      // Sort latencies for CDF
      std::sort(global_lat_vec.begin(), global_lat_vec.end());

      // Sample uniformly to get exactly target_samples points
      std::vector<double> sampled_latencies;
      if (total_samples <= target_samples) {
        // If we have fewer or equal samples, use all
        sampled_latencies = global_lat_vec;
      } else {
        // Uniform sampling: select indices 0, step, 2*step, ..., (target_samples-1)*step
        // This ensures we get min, max, and evenly distributed points
        sampled_latencies.reserve(target_samples);
        double step = static_cast<double>(total_samples - 1) / (target_samples - 1);
        for (size_t i = 0; i < target_samples; i++) {
          size_t idx = static_cast<size_t>(i * step + 0.5);  // Round to nearest
          if (idx >= total_samples) idx = total_samples - 1;  // Safety check
          sampled_latencies.push_back(global_lat_vec[idx]);
        }
      }

      // Write to file: one latency value per line (for CDF plotting)
      std::ofstream outfile(FLAGS_latency_output_file);
      if (outfile.is_open()) {
        for (size_t i = 0; i < sampled_latencies.size(); i++) {
          outfile << sampled_latencies[i] << "\n";
        }
        outfile.close();
        printf("Exported %zu latency samples (sampled from %zu total) to %s\n",
               sampled_latencies.size(), total_samples, FLAGS_latency_output_file.c_str());
      } else {
        fprintf(stderr, "Failed to open output file: %s\n",
                FLAGS_latency_output_file.c_str());
      }
    } else {
      printf("Warning: Only collected %zu latency samples (need at least %zu). "
             "Not exporting to file.\n", total_samples, target_samples);
    }
  }

  delete[] app_stats;
}
