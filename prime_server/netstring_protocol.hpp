#pragma once

#include <prime_server/prime_server.hpp>
#include <prime_server/zmq_helpers.hpp>

#include <cstdint>
#include <limits>

namespace prime_server {

struct netstring_request_info_t {
  uint32_t id;
  uint32_t time_stamp;

  void log(size_t response_size) const;
  bool keep_alive() const {
    return true;
  }
  explicit operator uint64_t() const {
    return static_cast<uint64_t>(id) | (static_cast<uint64_t>(time_stamp) << 32);
  }
};

struct netstring_entity_t {
  netstring_entity_t();
  netstring_request_info_t to_info(uint32_t id) const;
  std::string to_string() const;
  static std::string to_string(const std::string& message);
  static netstring_entity_t from_string(const char* start, size_t length);
  static const zmq::message_t& timeout(netstring_request_info_t& info);
  std::list<netstring_entity_t>
  from_stream(const char* start, size_t length, size_t max_size = std::numeric_limits<size_t>::max());
  void flush_stream();
  size_t size() const;
  void log(uint32_t id) const;

  struct request_exception_t {
    request_exception_t(const std::string& response);
    void log(uint32_t id) const;
    std::string response;
  };

  std::string body;
  size_t body_length;

  // TODO: fix this when we refactor to avoid subclassing the server
  std::list<uint64_t> enqueued;
};

class netstring_client_t : public client_t {
public:
  using client_t::client_t;

protected:
  virtual size_t stream_responses(const void* message, size_t size, bool& more);
  netstring_entity_t response;
};

using netstring_server_t = server_t<netstring_entity_t, netstring_request_info_t>;

shortcircuiter_t<netstring_entity_t> make_shortcircuiter(const std::string& health_check_str = "health_check");
} // namespace prime_server
