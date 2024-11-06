#ifndef KV_H
#define KV_H

#include <gflags/gflags.h>
#include <signal.h>
#include <unordered_map>
#include <optional>
#include "../apps_common.h"
#include "rpc.h"
#include "util/latency.h"
#include "util/numautils.h"
#include "util/timer.h"

static constexpr size_t kAppGetFgType = 1;
static constexpr size_t kAppGetBgType = 2;
static constexpr size_t kAppPutFgType = 3;
static constexpr size_t kAppPutBgType = 4;
static constexpr size_t kAppEvLoopMs = 500;

static constexpr size_t kAppMaxReqWindow = 1024;  // Max pending reqs per client

// Globals
volatile sig_atomic_t ctrl_c_pressed = 0;
void ctrl_c_handler(int) { ctrl_c_pressed = 1; }

// Flags
DEFINE_uint64(num_server_fg_threads, 0, "Number of server foreground threads");
DEFINE_uint64(num_server_bg_threads, 0, "Number of server background threads");
DEFINE_uint64(num_client_threads, 0, "Number of client threads");
DEFINE_uint64(req_window, 0, "Outstanding requests per client thread");
DEFINE_uint64(num_keys, 0, "Number of keys in the server's Hashmap");
DEFINE_uint64(get_req_percent, 0, "Percentage of get");
DEFINE_uint64(get_fg_percent, 0, "Percentage of get requests allocated to fg threads");
DEFINE_uint64(put_fg_percent, 0, "Percentage of put requests allocated to fg threads");

// Return true iff this machine is the one server
bool is_server() { return FLAGS_process_id == 0; }
/*
req data size = size_t + kKeySize + kValueSize = 88 bytes
resp data size = size_t + kValueSize = 72 bytes

req/resp pkt size : key size : value size
64                :
128               :
256               :
512               :
1024              : 
*/
class KV {
public:
  static constexpr size_t kKeySize = 16;
  static constexpr size_t kValueSize = 256;

  typedef struct {
    uint8_t key_[kKeySize];
  } key_t;
  
  typedef struct {
    uint8_t value_[kValueSize];
  } value_t;

  KV(size_t initial_size) {
    for(size_t i = 0; i < initial_size; i++){
      key_t key;
      size_t k = i;
      for(size_t t = 0; t < kKeySize; t++) {
        key.key_[t] = k & ((1<<8) - 1);
        k >>= 8;
      }
      value_t value;
      size_t v = i*0x12345+0x10501;
      for(size_t t = 0; t < kValueSize; t++) {
        value.value_[t] = v & ((1<<8) - 1);
        v >>= 8;
      }
      put(key, value);
    }
  }

  ~KV() {}

  bool put(key_t key, value_t value) {
    kvmap[key] = value;
    return true;
  }

  bool get(key_t key, value_t* value) {
    auto it = kvmap.find(key);

    if (it != kvmap.end()) {
      memcpy(value->value_, &(it->second), KV::kValueSize);
      return true;
    }

    return false;
  }

private:
  struct HashFunc {
    std::size_t operator()(const key_t &key) const {
      std::size_t hash = 0;
      for (size_t i = 0; i < kKeySize; i++) {
        hash = hash * 271 + key.key_[i];
      }
      return hash;
    }
  };
  struct CompareFunc {
    bool operator()(const key_t &key1, const key_t &key2) const {
      for(size_t i = 0; i < kKeySize; i++){
        if(key1.key_[i] != key2.key_[i]) return false;
      }
      return true;
    }
  };
  std::unordered_map<key_t, value_t, HashFunc, CompareFunc> kvmap;

};

// req size = size_t + kKeySize + kValueSize = 88 bytes
struct wire_req_t {
  size_t req_type;//kAppGetType or kAppPutType
  union {
    struct {
      KV::key_t key;
    } get_req;

    struct {
      KV::key_t key;
      KV::value_t value;
    } put_req;
  };

  std::string to_string() const {
    std::ostringstream ret;
    ret << "[Type " << ((req_type == kAppGetFgType || req_type == kAppGetBgType) ? "get" : "put")
        << ", key: ";
    if (req_type == kAppGetFgType || req_type == kAppGetBgType) {
      const uint64_t *key_64 =
          reinterpret_cast<const uint64_t *>(get_req.key.key_);
      for (size_t i = 0; i < KV::kKeySize / sizeof(uint64_t); i++) {
        ret << std::to_string(key_64[i]) << " ";
      }
    } else {
      const uint64_t *key_64 =
          reinterpret_cast<const uint64_t *>(put_req.key.key_);
      for (size_t i = 0; i < KV::kKeySize / sizeof(uint64_t); i++) {
        ret << std::to_string(key_64[i]) << " ";
      }
      ret << ", value: ";
      const uint64_t *value_64 =
          reinterpret_cast<const uint64_t *>(put_req.value.value_);
      for (size_t i = 0; i < KV::kValueSize / sizeof(uint64_t); i++) {
        ret << std::to_string(value_64[i]) << " ";
      }
    }
    ret << "]";
    return ret.str();
  }
};

//resp size = size_t + kValueSize = 72 bytes
enum class RespType : size_t { Success, Failure };
struct wire_resp_t {
  RespType resp_type;
  KV::value_t value;

  std::string to_string() const {
    std::ostringstream ret;
    ret << "[Value: ";
    for (size_t i = 0; i < KV::kValueSize; i++) {
      ret << std::to_string(value.value_[i]) << " ";
    }
    ret << "]";
    return ret.str();
  }
};

struct app_stats_t {
  double mrps;       // Point request rate
  double lat_us_50;  // Point request median latency
  double lat_us_90;  // Point request 90th percentile latency
  double lat_us_99;  // Point request 99th percentile latency
  size_t pad[4];

  app_stats_t() { memset(this, 0, sizeof(app_stats_t)); }

  static std::string get_template_str() {
    return "mrps lat_us_50 lat_us_90 lat_us_99";
  }

  std::string to_string() {
    return std::to_string(mrps) + " " + std::to_string(lat_us_50) + " " +
           std::to_string(lat_us_90) + " " + std::to_string(lat_us_99);
  }

  /// Accumulate stats
  app_stats_t &operator+=(const app_stats_t &rhs) {
    this->mrps += rhs.mrps;
    this->lat_us_50 += rhs.lat_us_50;
    this->lat_us_90 += rhs.lat_us_90;
    this->lat_us_99 += rhs.lat_us_99;
    return *this;
  }
};
static_assert(sizeof(app_stats_t) == 64, "");

// Per-thread application context
class AppContext : public BasicAppContext {
 public:
  struct {
    KV *kv = nullptr;
  } server;

  struct {
    erpc::ChronoTimer tput_timer;  // Throughput measurement start
    app_stats_t *app_stats;        // Common stats array for all threads

    erpc::Latency get_latency;
    erpc::Latency put_latency;

    struct {
      KV::key_t req_key;
      uint64_t req_ts_;
      erpc::MsgBuffer req_msgbuf_;
      erpc::MsgBuffer resp_msgbuf_;
    } window_[kAppMaxReqWindow];

    erpc::FastRand fast_rand;
    size_t num_resps_tot = 0;  // Total responses received (range & point reqs)
  } client;
};

// Allocate request and response MsgBuffers
void alloc_req_resp_msg_buffers(AppContext *c) {
  for (size_t msgbuf_idx = 0; msgbuf_idx < FLAGS_req_window; msgbuf_idx++) {
    c->client.window_[msgbuf_idx].req_msgbuf_ =
        c->rpc_->alloc_msg_buffer_or_die(sizeof(wire_req_t));

    c->client.window_[msgbuf_idx].resp_msgbuf_ =
        c->rpc_->alloc_msg_buffer_or_die(sizeof(wire_resp_t));
  }
}

#endif  // KV_H
