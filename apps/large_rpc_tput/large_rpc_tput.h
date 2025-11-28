#ifndef LARGE_RPC_TPUT_H
#define LARGE_RPC_TPUT_H

#include <gflags/gflags.h>
#include <signal.h>
#include "../apps_common.h"
#include "rpc.h"
#include "util/autorun_helpers.h"
#include "util/numautils.h"

static constexpr size_t kAppReqType = 1;
static constexpr uint8_t kAppDataByte = 3;  // Data transferred in req & resp
static constexpr size_t kAppMaxConcurrency = 8192;  // Outstanding reqs per thread

// Globals
volatile sig_atomic_t ctrl_c_pressed = 0;
void ctrl_c_handler(int) { ctrl_c_pressed = 1; }

// Flags
DEFINE_uint64(num_proc_0_threads, 0, "Threads in process 0");
DEFINE_uint64(num_proc_other_threads, 0, "Threads in process with ID != 0");
DEFINE_string(req_sizes, "", "Request data size");
DEFINE_string(resp_sizes, "", "Response data size");
DEFINE_string(concurrency, "", "Concurrent requests per thread");
DEFINE_uint64(app_ticks, 0, "add ticks when processing per packet");
DEFINE_double(drop_prob, 0, "Packet drop probability");
DEFINE_string(profile, "", "Experiment profile to use");
DEFINE_double(throttle, 0, "Throttle flows to incast receiver?");
DEFINE_double(throttle_fraction, 1, "Fraction of fair share to throttle to.");
DEFINE_string(latency_output_file, "", "Output file for latency data (CDF format)");

/// Return the req sizes of different threads.
std::vector<size_t> flags_get_req_sizes() {
  std::vector<size_t> ret;
  std::string req_str = FLAGS_req_sizes;
  if (req_str.size() == 0) return ret;

  std::vector<std::string> split_vec = erpc::split(req_str, ',');
  erpc::rt_assert(split_vec.size() > 0);

  for (auto &s : split_vec) ret.push_back(std::stoull(s));  // stoull trims ' '

  return ret;
}

/// Return the req sizes of different threads.
std::vector<size_t> flags_get_resp_sizes() {
  std::vector<size_t> ret;
  std::string resp_str = FLAGS_resp_sizes;
  if (resp_str.size() == 0) return ret;

  std::vector<std::string> split_vec = erpc::split(resp_str, ',');
  erpc::rt_assert(split_vec.size() > 0);

  for (auto &s : split_vec) ret.push_back(std::stoull(s));  // stoull trims ' '

  return ret;
}

/// Return the concurrency values of different threads.
std::vector<size_t> flags_get_concur() {
  std::vector<size_t> ret;
  std::string concur_str = FLAGS_concurrency;
  if (concur_str.size() == 0) return ret;

  std::vector<std::string> split_vec = erpc::split(concur_str, ',');
  erpc::rt_assert(split_vec.size() > 0);

  for (auto &s : split_vec) ret.push_back(std::stoull(s));  // stoull trims ' '

  return ret;
}

struct app_stats_t {
  double rx_gbps;
  double tx_gbps;
  size_t re_tx;
  double rtt_50_us;  // Median packet RTT
  double rtt_99_us;  // 99th percentile packet RTT
  double rpc_50_us;
  double rpc_99_us;
  double rpc_999_us;

  app_stats_t() { memset(this, 0, sizeof(app_stats_t)); }

  static std::string get_template_str() {
    return "rx_gbps tx_gbps re_tx rtt_50_us rtt_99_us rpc_50_us rpc_99_us "
           "rpc_999_us";
  }

  /// Return a space-separated string of all stats
  std::string to_string() {
    return std::to_string(rx_gbps) + " " + std::to_string(tx_gbps) + " " +
           std::to_string(re_tx) + " " + std::to_string(rtt_50_us) + " " +
           std::to_string(rtt_99_us) + " " + std::to_string(rpc_50_us) + " " +
           std::to_string(rpc_99_us) + " " + std::to_string(rpc_999_us);
  }

  /// Accumulate stats
  app_stats_t& operator+=(const app_stats_t& rhs) {
    this->rx_gbps += rhs.rx_gbps;
    this->tx_gbps += rhs.tx_gbps;
    this->re_tx += rhs.re_tx;
    this->rtt_50_us += rhs.rtt_50_us;
    this->rtt_99_us += rhs.rtt_99_us;
    this->rpc_50_us += rhs.rpc_50_us;
    this->rpc_99_us += rhs.rpc_99_us;
    this->rpc_999_us += rhs.rpc_999_us;
    return *this;
  }
};
static_assert(sizeof(app_stats_t) == 64, "");

// Per-thread application context
class AppContext : public BasicAppContext {
 public:
  // We need a wide range of latency measurements: ~4 us for 4KB RPCs, to
  // >10 ms for 8MB RPCs under congestion. So erpc::Latency doesn't work here.
  std::vector<double> lat_vec;  // Current interval latency samples
  std::vector<double> lat_vec_accumulated;  // Accumulated latency samples for export

  erpc::ChronoTimer tput_t0;  // Start time for throughput measurement
  app_stats_t* app_stats;     // Common stats array for all threads

  size_t stat_rx_bytes_tot = 0;  // Total bytes received
  size_t stat_tx_bytes_tot = 0;  // Total bytes transmitted

  uint64_t req_ts[kAppMaxConcurrency];  // Per-request timestamps
  erpc::MsgBuffer req_msgbuf[kAppMaxConcurrency];
  erpc::MsgBuffer resp_msgbuf[kAppMaxConcurrency];
};

// Allocate request and response MsgBuffers
void alloc_req_resp_msg_buffers(AppContext* c, size_t req_size, size_t resp_size, size_t concur) {
  for (size_t i = 0; i < concur; i++) {
    c->req_msgbuf[i] = c->rpc_->alloc_msg_buffer_or_die(req_size);
    c->resp_msgbuf[i] = c->rpc_->alloc_msg_buffer_or_die(resp_size);

    // Fill the request regardless of kAppMemset. This is a one-time thing.
    memset(c->req_msgbuf[i].buf_, kAppDataByte, req_size);
  }
}

#endif
