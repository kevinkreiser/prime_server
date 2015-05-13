#ifndef __NETSTRING_PROTOCOL_HPP__
#define __NETSTRING_PROTOCOL_HPP__

#include <prime_server/prime_server.hpp>
#include <prime_server/zmq_helpers.hpp>

#include <cstdint>

namespace prime_server {

  class netstring_client_t : public client_t {
   public:
    using client_t::client_t;
   protected:
    virtual size_t stream_responses(const void* message, size_t size, bool& more);
    std::string buffer;
  };

  class netstring_server_t : public server_t<std::string, uint64_t> {
   public:
    netstring_server_t(zmq::context_t& context, const std::string& client_endpoint, const std::string& proxy_endpoint, const std::string& result_endpoint, bool log = false);
    virtual ~netstring_server_t();
   protected:
    virtual void enqueue(const void* message, size_t size, const std::string& requester, std::string& buffer);
    virtual void dequeue(const uint64_t& request_info, size_t length);
    uint64_t request_id;
  };

  struct netstring_request_t {
    static void format(std::string& message);
  };

  struct netstring_response_t {
    static void format(std::string& message);
  };

}

#endif //__NETSTRING_PROTOCOL_HPP__
