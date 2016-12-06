#ifndef __PRIME_SERVER_HPP__
#define __PRIME_SERVER_HPP__

//some version info
#define PRIME_SERVER_VERSION_MAJOR 0
#define PRIME_SERVER_VERSION_MINOR 6
#define PRIME_SERVER_VERSION_PATCH 0

#include <functional>
#include <string>
#include <list>
#include <unordered_map>
#include <utility>
#include <type_traits>
#include <cstdint>
#include <ctime>

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
    server_t(zmq::context_t& context, const std::string& client_endpoint, const std::string& proxy_endpoint, const std::string& result_endpoint,
      const std::string& interrupt_endpoint, bool log = false, size_t max_request_size = DEFAULT_MAX_REQUEST_SIZE);
    virtual ~server_t();
    void serve();
   protected:
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
    //      send the request as a message to the proxy
    //      record the request with its id
    //      log the request if log == true
    virtual bool enqueue(const zmq::message_t& requester, const zmq::message_t& message, request_container_t& streaming_request) = 0;
    //implementing class shall:
    //  reply to the requester
    //  remove the outstanding request as it was either satisfied or timed-out
    //  may also remove the session depending on the original request or the result
    //  log the response if log == true
    virtual void dequeue(const std::list<zmq::message_t>& messages) = 0;

    //contractual obligations for supplying your own request_info_t, the last 2 are strict for the purposes
    //of possibly allowing the proxy to easily peak at the request id without knowing the server protocol
    static_assert(std::is_trivial<request_info_t>::value, "request_info_t must be trivial");
    static_assert(std::is_same<decltype(request_info_t().id), uint32_t>::value, "request_info_t::id must be uint32_t");
    static_assert(std::is_same<decltype(request_info_t().time_stamp), uint32_t>::value, "request_info_t::time_stamp must be uint32_t");
    static constexpr request_info_t sfinae_test_info{};
    static_assert(static_cast<const void*>(&sfinae_test_info) == static_cast<const void*>(&sfinae_test_info.id),
      "request_info_t::id must be the first member");
    static_assert(static_cast<const void*>(&sfinae_test_info.id + 1) == static_cast<const void*>(&sfinae_test_info.time_stamp),
      "request_info_t::time_stamp must be the second member");

    zmq::socket_t client;
    zmq::socket_t proxy;
    zmq::socket_t loopback;
    zmq::socket_t interrupt;

    bool log;
    size_t max_request_size;
    //a record of what open connections we have
    //TODO: keep time of last session activity and clear out stale sessions
    std::unordered_map<zmq::message_t, request_container_t> sessions;
    //a record of what requests we have in progress
    //TODO: keep time of request and kill requests that stick around for a long time
    std::unordered_map<uint64_t, zmq::message_t> requests;
  };

  //proxy messages between layers of a backend load balancing in between
  class proxy_t {
   public:
    //allows you to favor a certain heartbeat/worker for a given job
    using choose_function_t = std::function<const zmq::message_t* (const std::list<zmq::message_t>&, const std::list<zmq::message_t>&)>;
    proxy_t(zmq::context_t& context, const std::string& upstream_endpoint, const std::string& downstream_endpoint,
      const choose_function_t& choose_function = [](const std::list<zmq::message_t>&, const std::list<zmq::message_t>&){return nullptr;});
    virtual ~proxy_t();
    void forward();
   protected:
    virtual int expire();

    zmq::socket_t upstream;
    zmq::socket_t downstream;
    choose_function_t choose_function;

    //we want a fifo queue in the case that the proxy doesnt care what worker to send jobs to
    //having this constraint does also require that we store a bidirectional mapping between
    //worker addresses and their heartbeats. since heartbeats are application defined we only
    //store them once (they could be larger) and opt for storing the worker addresses duplicated
    std::list<zmq::message_t> fifo;
    std::unordered_map<zmq::message_t, std::list<zmq::message_t>::iterator> workers;
    std::unordered_map<const zmq::message_t*, zmq::message_t> heart_beats;
  };

  //get work from a load balancer proxy letting it know when you are idle
  class worker_t {
   public:
    //TODO: refactor this to allow streaming response (transfer-encoding: chunked)
    //might want to add another bool in here to signal that we need to call the work
    //function again until it somehow signals that its sending the last chunk
    struct result_t {
      bool intermediate;
      std::list<std::string> messages;
      std::string heart_beat;
    };
    using work_function_t = std::function<result_t (const std::list<zmq::message_t>&, void*)>;
    using cleanup_function_t = std::function<void ()>;

    worker_t(zmq::context_t& context, const std::string& upstream_proxy_endpoint, const std::string& downstream_proxy_endpoint,
      const std::string& result_endpoint, const std::string& interrupt_endpoint, const work_function_t& work_function,
      const cleanup_function_t& cleanup_function = [](){}, const std::string& heart_beat = "");
    virtual ~worker_t();
    void work();
   protected:
    void advertise();
    virtual void cancel(bool check_previous);

    zmq::socket_t upstream_proxy;
    zmq::socket_t downstream_proxy;
    zmq::socket_t loopback;
    zmq::socket_t interrupt;

    work_function_t work_function;
    cleanup_function_t cleanup_function;
    long heart_beat_interval;
    std::string heart_beat;
    uint64_t job;

    //keep a circular queue of the last x interrupts
    //when you get a new job search from newest backwards to see if its there
    //if so abort right away. if not subsequent calls to cancel will
    //either abort the job or push onto the circular queue
    //to check for interrupt just call recv with NO_WAIT

  };

}

#endif //__PRIME_SERVER_HPP__
