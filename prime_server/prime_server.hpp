#ifndef __PRIME_SERVER_HPP__
#define __PRIME_SERVER_HPP__

//some version info
#define PRIME_SERVER_VERSION_MAJOR 0
#define PRIME_SERVER_VERSION_MINOR 4
#define PRIME_SERVER_VERSION_PATCH 0


#include <functional>
#include <string>
#include <list>
#include <unordered_map>
#include <utility>
#include <type_traits>
#include <cstdint>

#include <prime_server/zmq_helpers.hpp>

/*
 * NOTE: ZMQ_STREAM sockets are 'raw' and require some extra work compared to other socket types
 *
 * - messages coalesce on the socket (makes sense given the name stream right). this means that
 *   you have to do work to break them back up into individual requests (protocol dependent)
 *
 * - there are buffers in zmq for both send (zmq::out_batch_size) and recv (zmq::in_batch_size).
 *   this means that you have to always be careful of partial messages and have some means of
 *   knowing when a message is whole or not.
 *
 */

namespace prime_server {

  //client makes requests and gets back responses in batches asynchronously
  class client_t {
   public:
    using request_function_t = std::function<std::pair<const void*, size_t> ()>;
    using collect_function_t = std::function<bool (const void*, size_t)>;

    client_t(zmq::context_t& context, const std::string& server_endpoint, const request_function_t& request_function,
      const collect_function_t& collect_function, size_t batch_size = 8912);
    virtual ~client_t();
    void batch();
   protected:
    virtual size_t stream_responses(const void*, size_t, bool&) = 0;

    zmq::socket_t server;
    request_function_t request_function;
    collect_function_t collect_function;
    size_t batch_size;
  };

  //server sits between a clients and a load balanced backend
  constexpr size_t DEFAULT_MAX_REQUEST_SIZE = 1024*1024*10;
  template <class request_container_t, class request_info_t>
  class server_t {
   public:
    static_assert(std::is_pod<request_info_t>::value, "server requires POD types for request info");
    server_t(zmq::context_t& context, const std::string& client_endpoint, const std::string& proxy_endpoint,
       const std::string& result_endpoint, bool log = false, size_t max_request_size = DEFAULT_MAX_REQUEST_SIZE);
    virtual ~server_t();
    void serve();
   protected:
    void handle_response(std::list<zmq::message_t>& messages);
    void handle_request(std::list<zmq::message_t>& messages);
    //implementing class shall:
    //  take the request_container pump more bytes into and get back out >= 0 whole request objects
    //  while there are bytes to process:
    //    if the request is malformed or too large
    //      signal the client socket appropriately
    //      return false if terminating the session/connection is desired
    //      log the request if log == true
    //    otherwise, for each whole request:
    //      send the requester as a message to the proxy
    //      send the request id as a message to the proxy
    //      send the request as a message to the proxy
    //      record the request with its id
    //      log the request if log == true
    virtual bool enqueue(const void* bytes, size_t length, const std::string& requester, request_container_t& streaming_request) = 0;
    //implementing class shall:
    //  remove the outstanding request as it was either satisfied, timed-out
    //  depending on the original request or the result the session may also be terminated
    //  log the response if log == true
    virtual void dequeue(const request_info_t& request_info, size_t length) = 0;

    zmq::socket_t client;
    zmq::socket_t proxy;
    zmq::socket_t loopback;
    bool log;
    size_t max_request_size;
    //a record of what open connections we have
    //TODO: keep time of last session activity and clear out stale sessions
    std::unordered_map<std::string, request_container_t> sessions;
    //a record of what requests we have in progress
    //TODO: keep time of request and kill requests that stick around for a long time
    std::unordered_map<uint64_t, std::string> requests;
  };

  //proxy messages between layers of a backend load balancing in between
  class proxy_t {
   public:
    proxy_t(zmq::context_t& context, const std::string& upstream_endpoint, const std::string& downstream_endpoint);
    void forward();
   protected:
    zmq::socket_t upstream;
    zmq::socket_t downstream;
  };

  //get work from a load balancer proxy letting it know when you are idle
  class worker_t {
   public:
    //TODO: refactor this to allow streaming response (transfer-encoding: chunked)
    //might want to add another bool in here to single that we need to call the work
    //function again until it somehow signals that its sending the last chunk
    struct result_t {
      bool intermediate;
      std::list<std::string> messages;
    };
    using work_function_t = std::function<result_t (const std::list<zmq::message_t>&, void*)>;
    using cleanup_function_t = std::function<void ()>;

    worker_t(zmq::context_t& context, const std::string& upstream_proxy_endpoint, const std::string& downstream_proxy_endpoint,
      const std::string& result_endpoint, const work_function_t& work_function, const cleanup_function_t& cleanup_function = [](){});
    void work();
   protected:
    void advertise();
    zmq::socket_t upstream_proxy;
    zmq::socket_t downstream_proxy;
    zmq::socket_t loopback;
    work_function_t work_function;
    cleanup_function_t cleanup_function;
    long heart_beat;
  };

}

#endif //__PRIME_SERVER_HPP__
