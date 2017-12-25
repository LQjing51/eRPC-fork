#ifndef ERPC_AUTORUN_HELPERS_H
#define ERPC_AUTORUN_HELPERS_H

#include <boost/algorithm/string.hpp>
#include <fstream>
#include <string>
#include <vector>
#include "common.h"

namespace erpc {

// Return line with index \p n from \p filename. Throw exception if it doesn't
// exist.
static std::string get_line_n(std::string filename, size_t n) {
  std::ifstream in(filename.c_str());

  std::string s;
  s.reserve(100);  // For performance

  for (size_t i = 0; i < n; i++) {
    std::getline(in, s);
    erpc::rt_assert(!s.empty(), "Insufficient lines in " + filename);
  }

  std::getline(in, s);
  erpc::rt_assert(!s.empty(), "Insufficient lines in " + filename);

  return s;
}

static std::string extract_hostname_from_remote(std::string remote) {
  std::vector<std::string> split_vec;
  boost::split(split_vec, remote, boost::is_any_of(":"));
  erpc::rt_assert(split_vec.size() == 2 && split_vec[0].length() > 0 &&
                      split_vec[1].length() > 0,
                  "Invalid remote " + remote);

  return split_vec[0];  // Column 0
}

static std::string extract_udp_port_from_remote(std::string remote) {
  std::vector<std::string> split_vec;
  boost::split(split_vec, remote, boost::is_any_of(":"));
  erpc::rt_assert(split_vec.size() == 2 && split_vec[0].length() > 0 &&
                      split_vec[1].length() > 0,
                  "Invalid remote " + remote);

  return split_vec[1];  // Column 1
}

// Return the hostname of the process with index process_i, from the autorun
// processes file.
static std::string get_hostname_for_process(size_t process_i) {
  std::string process_file = "../eRPC/scripts/autorun_process_file";
  std::string line = get_line_n(process_file, process_i);
  return extract_hostname_from_remote(line);
}

// Return the UDP port of the process with index process_i, from the autorun
// processes file.
static std::string get_udp_port_for_process(size_t process_i) {
  std::string process_file = "../eRPC/scripts/autorun_process_file";
  std::string line = get_line_n(process_file, process_i);
  return extract_udp_port_from_remote(line);
}

}  // End erpc

#endif  // ERPC_AUTORUN_HELPERS_H