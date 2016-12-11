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
#include <unordered_set>
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

  constexpr size_t DEFAULT_MAX_REQUEST_SIZE = 1024*1024*10; //10 megabytes
  constexpr uint32_t DEFAULT_REQUEST_TIMEOUT = -1;          //infinity seconds

  //TODO: bundle both request_containter_t (req, rep) and request_info_t into
  //a single session_t that implements all the guts of the protocol

  //server sits between a clients and a load balanced backend
  template <class request_container_t, class request_info_t>
  class server_t {
   public:
    server_t(zmq::context_t& context, const std::string& client_endpoint, const std::string& proxy_endpoint, const std::string& result_endpoint,
      const std::string& interrupt_endpoint, bool log = false, size_t max_request_size = DEFAULT_MAX_REQUEST_SIZE, uint32_t request_timeout = DEFAULT_REQUEST_TIMEOUT);
    virtual ~server_t();
    void serve();
   protected:
    void handle_request(std::list<zmq::message_t>& messages);
    virtual bool enqueue(const zmq::message_t& requester, const zmq::message_t& message, request_container_t& streaming_request);
    virtual bool dequeue(const request_info_t& info, const zmq::message_t& response);
    void handle_timeouts();

    //contractual obligations for supplying your own request_info_t, the last 2 are strict for the purposes
    //of allowing the server/proxy/worker to easily peak at the request id and time stamp without knowing the protocol
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
    uint32_t request_timeout;
    uint32_t request_id;

    //a record of what open connections we have
    //TODO: keep time of last session activity and clear out stale sessions
    std::unordered_map<zmq::message_t, request_container_t> sessions;
    //a record of what requests we have in progress
    std::unordered_map<uint64_t, zmq::message_t> requests;
    //order list of requests
    std::list<request_info_t> request_history;
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
    using interrupt_function_t = std::function<void ()>;
    using work_function_t = std::function<result_t (const std::list<zmq::message_t>&, void*, interrupt_function_t&)>;
    using cleanup_function_t = std::function<void ()>;

    worker_t(zmq::context_t& context, const std::string& upstream_proxy_endpoint, const std::string& downstream_proxy_endpoint,
      const std::string& result_endpoint, const std::string& interrupt_endpoint, const work_function_t& work_function,
      const cleanup_function_t& cleanup_function = [](){}, const std::string& heart_beat = "");
    virtual ~worker_t();
    void work();
   protected:
    void advertise();
    virtual void handle_interrupt(bool force_check);

    zmq::socket_t upstream_proxy;
    zmq::socket_t downstream_proxy;
    zmq::socket_t loopback;
    zmq::socket_t interrupt;

    work_function_t work_function;
    cleanup_function_t cleanup_function;
    long heart_beat_interval;
    std::string heart_beat;
    uint64_t job;
    std::unordered_set<uint64_t> interrupts;
    std::list<uint64_t> interrupt_history;
  };

}

#endif //__PRIME_SERVER_HPP__
