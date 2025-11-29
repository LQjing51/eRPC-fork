#include "kv.h"
#include <signal.h>
#include <cstring>
#include "util/autorun_helpers.h"

void app_cont_func(void *, void *);  // Forward declaration

static constexpr bool kAppVerbose = false;

void get_req_handler(erpc::ReqHandle *req_handle, void *_context) {
  auto *c = static_cast<AppContext *>(_context);

  const size_t etid = c->rpc_->get_etid();

  KV *kv = c->server.kv;
  assert(kv != nullptr);

  const auto *req_msgbuf = req_handle->get_req_msgbuf();
  assert(req_msgbuf->get_data_size() == sizeof(wire_req_t));

  auto *req = reinterpret_cast<wire_req_t *>(req_msgbuf->buf_);

  erpc::Rpc<erpc::CTransport>::resize_msg_buffer(&req_handle->pre_resp_msgbuf_,
                                                sizeof(wire_resp_t));
  auto *resp =
      reinterpret_cast<wire_resp_t *>(req_handle->pre_resp_msgbuf_.buf_);
  memset(&(resp->value), 0, KV::kValueSize);
  const bool success = kv->get(req->get_req.key, &(resp->value));
  resp->resp_type = success ? RespType::Success : RespType::Failure;
  // printf("%d, key %ld value: %ld\n", success, *reinterpret_cast<size_t*>(req->get_req.key.key_), *reinterpret_cast<size_t*>(resp->value.value_));

  if (kAppVerbose) {
    printf(
        "main: Handled point request in eRPC thread %zu. Key %s, found %s, "
        "value %s\n",
        etid, req->to_string().c_str(), success ? "yes" : "no",
        success ? resp->to_string().c_str() : "N/A");
  }

  c->rpc_->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);
}

void scan_req_handler(erpc::ReqHandle *req_handle, void *_context) {
  auto *c = static_cast<AppContext *>(_context);

  const size_t etid = c->rpc_->get_etid();

  KV *kv = c->server.kv;
  assert(kv != nullptr);

  const auto *req_msgbuf = req_handle->get_req_msgbuf();
  assert(req_msgbuf->get_data_size() == sizeof(wire_req_t));

  auto *req = reinterpret_cast<wire_req_t *>(req_msgbuf->buf_);

  erpc::Rpc<erpc::CTransport>::resize_msg_buffer(&req_handle->pre_resp_msgbuf_,
                                                sizeof(wire_resp_t));
  auto *resp =
      reinterpret_cast<wire_resp_t *>(req_handle->pre_resp_msgbuf_.buf_);
  memset(&(resp->value), 0, KV::kValueSize);
  const bool success = kv->scan(req->get_req.key, &(resp->value));
  resp->resp_type = success ? RespType::Success : RespType::Failure;
  // printf("%d, key %ld value: %ld\n", success, *reinterpret_cast<size_t*>(req->get_req.key.key_), *reinterpret_cast<size_t*>(resp->value.value_));

  if (kAppVerbose) {
    printf(
        "main: Handled point request in eRPC thread %zu. Key %s, found %s, "
        "value %s\n",
        etid, req->to_string().c_str(), success ? "yes" : "no",
        success ? resp->to_string().c_str() : "N/A");
  }

  c->rpc_->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);
}


void put_req_handler(erpc::ReqHandle *req_handle, void *_context) {
  auto *c = static_cast<AppContext *>(_context);

  const size_t etid = c->rpc_->get_etid();

  KV *kv = c->server.kv;
  assert(kv != nullptr);

  const auto *req_msgbuf = req_handle->get_req_msgbuf();
  assert(req_msgbuf->get_data_size() == sizeof(wire_req_t));

  auto *req = reinterpret_cast<const wire_req_t *>(req_msgbuf->buf_);

  erpc::Rpc<erpc::CTransport>::resize_msg_buffer(&req_handle->pre_resp_msgbuf_,
                                                 sizeof(wire_resp_t));
  auto *resp =
      reinterpret_cast<wire_resp_t *>(req_handle->pre_resp_msgbuf_.buf_);
  //TODO QIJING
  const bool success = kv->put(req->put_req.key, req->put_req.value);
  resp->resp_type = success ? RespType::Success : RespType::Failure;

  if (kAppVerbose) {
    printf("main: Handling range request in eRPC thread %zu.\n", etid);
  }

  c->rpc_->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);
}

// Send one request using this MsgBuffer
void send_req(AppContext *c, size_t msgbuf_idx) {
  erpc::MsgBuffer &req_msgbuf = c->client.window_[msgbuf_idx].req_msgbuf_;
  assert(req_msgbuf.get_data_size() == sizeof(wire_req_t));

  // Generate a random request
  wire_req_t req;
  uint32_t rand_num = c->fastrand_.next_u32() % 100;
  if (rand_num < FLAGS_get_req_percent) {
    if (c->fastrand_.next_u32() % 100 < FLAGS_get_fg_percent) {
      req.req_type = kAppGetFgType;
    } else {
      req.req_type = kAppGetBgType;
    }
    /*
    ensure the key size is larger or equal to 4 Bytes.
    choose key randomly, key range is [0, FLAGS_num_keys].
    Only Map[0-FLAGS_num_keys][] will be touched no matter key size.
    */
    memset(&(req.get_req.key.key_), 0, KV::kKeySize);
    uint32_t *key_32 = reinterpret_cast<uint32_t *>(req.get_req.key.key_);
    *key_32 = c->fastrand_.next_u32() % FLAGS_num_keys;

  } else if (rand_num < FLAGS_scan_req_percent) {
    req.req_type = kAppScanType;
    /*
    ensure the key size is larger or equal to 4 Bytes.
    choose key randomly, key range is [0, FLAGS_num_keys].
    Only Map[0-FLAGS_num_keys][] will be touched no matter key size.
    */
    memset(&(req.get_req.key.key_), 0, KV::kKeySize);
    uint32_t *key_32 = reinterpret_cast<uint32_t *>(req.get_req.key.key_);
    *key_32 = c->fastrand_.next_u32() % FLAGS_num_keys;

  } else {
    if (c->fastrand_.next_u32() % 100 < FLAGS_put_fg_percent) {
      req.req_type = kAppPutFgType;
    } else {
      req.req_type = kAppPutBgType;
    }

    /*
    ensure the key size is larger or equal to 4 Bytes.
    choose key randomly, key range is [0, FLAGS_num_keys].
    Only Map[0-FLAGS_num_keys][] will be touched no matter key size.
    */
    memset(&(req.put_req.key.key_), 0, KV::kKeySize);
    uint32_t *key_32 = reinterpret_cast<uint32_t *>(req.put_req.key.key_);
    *key_32 = c->fastrand_.next_u32() % FLAGS_num_keys;
    
    /*ensure the value size is larger or equal to 8 Bytes.*/
    memset(&(req.put_req.value.value_), 0, KV::kValueSize);
    uint64_t *value_64 = reinterpret_cast<uint64_t *>(req.put_req.value.value_);
    *value_64 = (static_cast<uint64_t>(*key_32))*(0x12345)+0x10501;
  }

  *reinterpret_cast<wire_req_t *>(req_msgbuf.buf_) = req;

  c->client.window_[msgbuf_idx].req_key = (req.req_type == kAppGetFgType || req.req_type == kAppGetBgType || req.req_type == kAppScanType)? req.get_req.key : req.put_req.key;
  c->client.window_[msgbuf_idx].req_ts_ = erpc::rdtsc();

  if (kAppVerbose) {
    printf("main: Enqueuing request with msgbuf_idx %zu.\n", msgbuf_idx);
    sleep(1);
  }

  c->rpc_->enqueue_request(0, req.req_type, &req_msgbuf,
                           &c->client.window_[msgbuf_idx].resp_msgbuf_,
                           app_cont_func, reinterpret_cast<void *>(msgbuf_idx));
}

void app_cont_func(void *_context, void *_msgbuf_idx) {
  auto *c = static_cast<AppContext *>(_context);
  const auto msgbuf_idx = reinterpret_cast<size_t>(_msgbuf_idx);
  if (kAppVerbose) {
    printf("main: Received response for msgbuf %zu.\n", msgbuf_idx);
  }

  const auto &resp_msgbuf = c->client.window_[msgbuf_idx].resp_msgbuf_;
  erpc::rt_assert(resp_msgbuf.get_data_size() == sizeof(wire_resp_t),
                  "Invalid response size");

  const double usec =
      erpc::to_usec(erpc::rdtsc() - c->client.window_[msgbuf_idx].req_ts_,
                    c->rpc_->get_freq_ghz());
  assert(usec >= 0);

  const auto *req = reinterpret_cast<wire_req_t *>(
      c->client.window_[msgbuf_idx].req_msgbuf_.buf_);

  if (req->req_type == kAppGetFgType || req->req_type == kAppGetBgType) {
    // Store latency sample in vector (similar to large_rpc_tput)
    c->client.get_lat_vec.push_back(usec);

    {
      const auto *wire_resp = reinterpret_cast<wire_resp_t *>(resp_msgbuf.buf_);
      const uint64_t* recvd_value = reinterpret_cast<const uint64_t *>(wire_resp->value.value_);
      const uint32_t* key = reinterpret_cast<const uint32_t *>(c->client.window_[msgbuf_idx].req_key.key_);
      uint64_t true_value = static_cast<uint64_t>(*key)*0x12345+0x10501;
      if (true_value != *recvd_value) {
        fprintf(stderr,
                "KV Get Value mismatch. Req key = %u, recvd_value = %lu, true_value = %lu\n",
                *key, *recvd_value, true_value);
      }
    }
  } else if (req->req_type == kAppScanType) {
    // Store latency sample in vector
    c->client.scan_lat_vec.push_back(usec);
    // Check the value
    {
      const auto *wire_resp = reinterpret_cast<wire_resp_t *>(resp_msgbuf.buf_);
      const uint64_t* recvd_value = reinterpret_cast<const uint64_t *>(wire_resp->value.value_);
      const uint32_t key_value = *reinterpret_cast<const uint32_t *>(c->client.window_[msgbuf_idx].req_key.key_);
      uint32_t tmp_key = key_value;
      uint64_t scan_value = 0;
      for(int i = 0; i < 128 && tmp_key < FLAGS_num_keys; i++, tmp_key++) {
        scan_value += static_cast<uint64_t>(tmp_key)*0x12345+0x10501;
      }
      if (scan_value != *recvd_value) {
        fprintf(stderr, "KV Scan Value mismatch. Req key = %u, recvd_value = %lu, true_value = %lu\n", key_value, *recvd_value, scan_value);
      }
    }
  } else {
    // This should be a put request (kAppPutFgType or kAppPutBgType)
    // Store latency sample in vector
    c->client.put_lat_vec.push_back(usec);
    // Qijing TODO: add success check?
  }

  c->client.num_resps_tot++;
  send_req(c, msgbuf_idx);
}

void client_print_stats(AppContext &c) {
  const double seconds = c.client.tput_timer.get_us() / 1e6;
  const double tput_mrps = c.client.num_resps_tot / (seconds * 1000000);
  app_stats_t &stats = c.client.app_stats[c.thread_id_];
  stats.mrps = tput_mrps;
  double get_lat_50 = 0, get_lat_90 = 0, get_lat_99 = 0;
  double put_lat_50 = 0, put_lat_90 = 0, put_lat_99 = 0;
  double scan_lat_50 = 0, scan_lat_90 = 0, scan_lat_99 = 0;
  if (c.client.get_lat_vec.size() > 0) {
    std::sort(c.client.get_lat_vec.begin(), c.client.get_lat_vec.end());
    get_lat_50 = c.client.get_lat_vec.at(c.client.get_lat_vec.size() * 0.50);
    get_lat_90 = c.client.get_lat_vec.at(c.client.get_lat_vec.size() * 0.90);
    get_lat_99 = c.client.get_lat_vec.at(c.client.get_lat_vec.size() * 0.99);
  }
  if (c.client.put_lat_vec.size() > 0) {
    std::sort(c.client.put_lat_vec.begin(), c.client.put_lat_vec.end());
    put_lat_50 = c.client.put_lat_vec.at(c.client.put_lat_vec.size() * 0.50);
    put_lat_90 = c.client.put_lat_vec.at(c.client.put_lat_vec.size() * 0.90);
    put_lat_99 = c.client.put_lat_vec.at(c.client.put_lat_vec.size() * 0.99);
  }
  if (c.client.scan_lat_vec.size() > 0) {
    std::sort(c.client.scan_lat_vec.begin(), c.client.scan_lat_vec.end());
    scan_lat_50 = c.client.scan_lat_vec.at(c.client.scan_lat_vec.size() * 0.50);
    scan_lat_90 = c.client.scan_lat_vec.at(c.client.scan_lat_vec.size() * 0.90);
    scan_lat_99 = c.client.scan_lat_vec.at(c.client.scan_lat_vec.size() * 0.99);
  }
  printf(
      "Client %zu. Tput = %.3f Mrps. "
      "Get-query latency (us) = {%.1f 50th, %.1f 90th, %.1f 99th}. "
      "Put-query latency (us) = {%.1f 50th, %.1f 90th, %.1f 99th}. "
      "Scan-query latency (us) = {%.1f 50th, %.1f 90th, %.1f 99th}.\n",
      c.thread_id_, tput_mrps, get_lat_50, get_lat_90, get_lat_99, put_lat_50, put_lat_90, put_lat_99, scan_lat_50, scan_lat_90, scan_lat_99);

  c.client.num_resps_tot = 0;
  c.client.get_lat_vec.clear();
  c.client.put_lat_vec.clear();
  c.client.scan_lat_vec.clear();
  c.client.tput_timer.reset();
}

void client_thread_func(size_t thread_id, app_stats_t *app_stats,
                        erpc::Nexus *nexus) {
  AppContext c;
  c.thread_id_ = thread_id;
  c.client.app_stats = app_stats;

  std::vector<size_t> port_vec = flags_get_numa_ports(FLAGS_numa_node);
  erpc::rt_assert(port_vec.size() > 0);
  const uint8_t phy_port = port_vec.at(thread_id % port_vec.size());

  erpc::Rpc<erpc::CTransport> rpc(nexus, static_cast<void *>(&c),
                                  static_cast<uint8_t>(thread_id),
                                  basic_sm_handler, phy_port);
  rpc.retry_connect_on_invalid_rpc_id_ = true;
  c.rpc_ = &rpc;

  // Each client creates a session to only one server thread
  const size_t client_gid =
      (FLAGS_process_id * FLAGS_num_client_threads) + thread_id;
  const size_t server_tid =
      client_gid % FLAGS_num_server_fg_threads;  // eRPC TID

  c.session_num_vec_.resize(1);
  c.session_num_vec_[0] =
      rpc.create_session(erpc::get_uri_for_process(0), server_tid);
  assert(c.session_num_vec_[0] >= 0);

  while (c.num_sm_resps_ != 1) {
    rpc.run_event_loop(200);  // 200 milliseconds
    if (ctrl_c_pressed == 1) return;
  }
  assert(c.rpc_->is_connected(c.session_num_vec_[0]));
  fprintf(stderr, "main: Thread %zu: Connected. Sending requests.\n",
          thread_id);

  alloc_req_resp_msg_buffers(&c);
  c.client.tput_timer.reset();
  for (size_t i = 0; i < FLAGS_req_window; i++) send_req(&c, i);

  for (size_t i = 0; i < FLAGS_test_ms; i += kAppEvLoopMs) {
    c.rpc_->run_event_loop(kAppEvLoopMs);
    if (ctrl_c_pressed == 1) break;
    client_print_stats(c);
  }
}

void server_thread_func(size_t thread_id, erpc::Nexus *nexus) {
  AppContext c;
  c.thread_id_ = thread_id;

  KV* kv = new KV(FLAGS_num_keys);
  c.server.kv = kv;
  printf("finished initial KV\n");

  std::vector<size_t> port_vec = flags_get_numa_ports(FLAGS_numa_node);
  erpc::rt_assert(port_vec.size() > 0);
  uint8_t phy_port = port_vec.at(thread_id % port_vec.size());

  erpc::Rpc<erpc::CTransport> rpc(nexus, static_cast<void *>(&c),
                                  static_cast<uint8_t>(thread_id),
                                  basic_sm_handler, phy_port);
  c.rpc_ = &rpc;
  while (ctrl_c_pressed == 0) rpc.run_event_loop(200);
}

int main(int argc, char **argv) {
  signal(SIGINT, ctrl_c_handler);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  erpc::rt_assert(FLAGS_req_window <= kAppMaxReqWindow, "Invalid req window");
  erpc::rt_assert(FLAGS_get_req_percent <= 100, "Invalid get req percent");

  if (FLAGS_num_server_bg_threads == 0) {
    printf(
        "main: Warning: No background threads. "
        "Range queries will run in foreground.\n");
  }

  if (is_server()) {
    erpc::rt_assert(FLAGS_process_id == 0, "Invalid server process ID");

    // eRPC stuff
    erpc::Nexus nexus(erpc::get_uri_for_process(FLAGS_process_id),
                      FLAGS_numa_node, FLAGS_num_server_bg_threads);

    if ((FLAGS_get_fg_percent != 100) || (FLAGS_get_fg_percent != 100)) {
      erpc::rt_assert(FLAGS_num_server_bg_threads > 0, "Need bg threads to handle bg requests");
    }
    
    nexus.register_req_func(kAppGetFgType, get_req_handler,
                            erpc::ReqFuncType::kForeground);

    nexus.register_req_func(kAppPutFgType, put_req_handler,
                            erpc::ReqFuncType::kForeground);

    nexus.register_req_func(kAppScanType, scan_req_handler,
                            erpc::ReqFuncType::kForeground);

    if (FLAGS_num_server_bg_threads > 0) {
      nexus.register_req_func(kAppGetBgType, get_req_handler,
                            erpc::ReqFuncType::kBackground);

      nexus.register_req_func(kAppPutBgType, put_req_handler,
                            erpc::ReqFuncType::kBackground);
    }
    std::vector<std::thread> thread_arr(FLAGS_num_server_fg_threads);
    for (size_t i = 0; i < FLAGS_num_server_fg_threads; i++) {
      thread_arr[i] = std::thread(server_thread_func, i, &nexus);
      erpc::bind_to_core(thread_arr[i], FLAGS_numa_node, i);
    }

    for (auto &thread : thread_arr) thread.join();
  } else {
    erpc::rt_assert(FLAGS_process_id > 0, "Invalid process ID");
    erpc::Nexus nexus(erpc::get_uri_for_process(FLAGS_process_id),
                      FLAGS_numa_node, FLAGS_num_server_bg_threads);

    std::vector<std::thread> thread_arr(FLAGS_num_client_threads);
    auto *app_stats = new app_stats_t[FLAGS_num_client_threads];
    for (size_t i = 0; i < FLAGS_num_client_threads; i++) {
      thread_arr[i] = std::thread(client_thread_func, i, app_stats, &nexus);
      erpc::bind_to_core(thread_arr[i], FLAGS_numa_node, i);
    }

    for (auto &thread : thread_arr) thread.join();
  }
}
