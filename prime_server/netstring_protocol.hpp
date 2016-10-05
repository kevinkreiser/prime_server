#ifndef __NETSTRING_PROTOCOL_HPP__
#define __NETSTRING_PROTOCOL_HPP__

#include <prime_server/prime_server.hpp>
#include <prime_server/zmq_helpers.hpp>

#include <cstdint>
#include <limits>

namespace prime_server {

  struct netstring_entity_t {
    netstring_entity_t();
    std::string to_string() const;
    static std::string to_string(const std::string& message);
    static netstring_entity_t from_string(const char* start, size_t length);
    std::list<netstring_entity_t> from_stream(const char* start, size_t length, size_t max_size = std::numeric_limits<size_t>::max());
    void flush_stream();
    size_t size() const;

    std::string body;
    size_t body_length;
  };

  class netstring_client_t : public client_t {
   public:
    using client_t::client_t;
   protected:
    virtual size_t stream_responses(const void* message, size_t size, bool& more);
    netstring_entity_t response;
  };

  class netstring_server_t : public server_t<netstring_entity_t, uint64_t> {
   public:
    netstring_server_t(zmq::context_t& context, const std::string& client_endpoint, const std::string& proxy_endpoint,
                       const std::string& result_endpoint, bool log = false, size_t max_request_size = DEFAULT_MAX_REQUEST_SIZE);
    virtual ~netstring_server_t();
   protected:
    virtual bool enqueue(const zmq::message_t& requester, const zmq::message_t& message, netstring_entity_t& buffer);
    virtual void dequeue(const uint64_t& request_info, size_t length);
    uint64_t request_id;
  };

}

#endif //__NETSTRING_PROTOCOL_HPP__
