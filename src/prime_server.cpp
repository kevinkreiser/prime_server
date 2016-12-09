#include <queue>
#include <deque>
#include <unordered_set>
#include <stdexcept>

#include "prime_server.hpp"
#include "netstring_protocol.hpp"
#include "http_protocol.hpp"
#include "logging.hpp"

namespace {
  struct interrupt_t : public std::runtime_error {
    interrupt_t(uint32_t id): std::runtime_error("Request " + std::to_string(id) + " was interrupted") {}
  };
}

namespace prime_server {

  client_t::client_t(zmq::context_t& context, const std::string& server_endpoint, const request_function_t& request_function,
    const collect_function_t& collect_function, size_t batch_size):
    server(context, ZMQ_STREAM), request_function(request_function), collect_function(collect_function), batch_size(batch_size) {

    int disabled = 0;
    server.setsockopt(ZMQ_SNDHWM, &disabled, sizeof(disabled));
    server.setsockopt(ZMQ_RCVHWM, &disabled, sizeof(disabled));
    server.connect(server_endpoint.c_str());
  }
  client_t::~client_t(){}
  void client_t::batch() {
    //swallow the first response as its just for connecting
    auto connection = server.recv_all(0);
    if(connection.size() != 2 || connection.front().size() == 0 || connection.back().size() != 0)
      throw std::logic_error("Connection should have garnered an identity frame followed by a blank message");

    //need the identity to identify our connection when we send stuff
    uint8_t identity[256];
    size_t identity_size = sizeof(identity);
    server.getsockopt(ZMQ_IDENTITY, identity, &identity_size);

    bool more;
    do {
      //request some
      size_t current_batch = 0;
      while(current_batch++ < batch_size) {
        try {
          //see if we are still making stuff
          auto request = request_function();
          if(request.second == 0)
            break;
          //send the stuff on
          server.send(static_cast<const void*>(identity), identity_size, ZMQ_SNDMORE);
          server.send(request.first, request.second, 0);
        }
        catch(const std::exception& e) {
          logging::ERROR(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " client_t: " + e.what());
        }
      }

      //receive some
      more = false;
      current_batch = 0;
      while(current_batch < batch_size) {
        try {
          //see if we are still waiting for stuff
          auto messages = server.recv_all(0);
          messages.pop_front();
          current_batch += stream_responses(messages.front().data(), messages.front().size(), more);
        }
        catch(const std::exception& e) {
          logging::ERROR(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " client_t: " + e.what());
        }
        if(!more)
          break;
      }
    //keep going while we expect more results
    }while(more);
  }

  template <class request_container_t, class request_info_t>
  server_t<request_container_t, request_info_t>::server_t(zmq::context_t& context, const std::string& client_endpoint, const std::string& proxy_endpoint,
    const std::string& result_endpoint, const std::string& interrupt_endpoint, bool log, size_t max_request_size):
    client(context, ZMQ_STREAM), proxy(context, ZMQ_DEALER), loopback(context, ZMQ_SUB), interrupt(context, ZMQ_PUB),
    log(log), max_request_size(max_request_size), request_id(0) {

    int disabled = 0;
    client.setsockopt(ZMQ_SNDHWM, &disabled, sizeof(disabled));
    client.setsockopt(ZMQ_RCVHWM, &disabled, sizeof(disabled));
    client.bind(client_endpoint.c_str());

    proxy.setsockopt(ZMQ_RCVHWM, &disabled, sizeof(disabled));
    proxy.setsockopt(ZMQ_SNDHWM, &disabled, sizeof(disabled));
    proxy.connect(proxy_endpoint.c_str());

    //TODO: consider making this into a pull socket so we dont lose any results due to timing
    loopback.setsockopt(ZMQ_RCVHWM, &disabled, sizeof(disabled));
    loopback.setsockopt(ZMQ_SUBSCRIBE, "", 0);
    loopback.bind(result_endpoint.c_str());

    interrupt.setsockopt(ZMQ_SNDHWM, &disabled, sizeof(disabled));
    interrupt.bind(interrupt_endpoint.c_str());

    sessions.reserve(1024);
    requests.reserve(1024);
  }

  template <class request_container_t, class request_info_t>
  server_t<request_container_t, request_info_t>::~server_t(){}

  template <class request_container_t, class request_info_t>
  void server_t<request_container_t, request_info_t>::serve() {
    while(true) {
      //check for activity on the client socket and the result socket
      //TODO: set a timeout based on session inactivity timeout or request timeout
      zmq::pollitem_t items[] = { { loopback, 0, ZMQ_POLLIN, 0 }, { client, 0, ZMQ_POLLIN, 0 } };
      zmq::poll(items, 2, -1);

      //got a new result
      if(items[0].revents & ZMQ_POLLIN) {
        try {
          auto messages = loopback.recv_all(ZMQ_DONTWAIT);
          //reply to client and cleanup request or session
          dequeue(messages);
        }
        catch(const std::exception& e) {
          logging::ERROR(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " server_t: " + e.what());
        }
      }

      //got a new request
      if(items[1].revents & ZMQ_POLLIN) {
        try {
          auto messages = client.recv_all(ZMQ_DONTWAIT);
          handle_request(messages);
        }
        catch(const std::exception& e) {
          logging::ERROR(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " server_t: " + e.what());
        }
      }

      //TODO: kill stale sessions
    }
  }

  template <class request_container_t, class request_info_t>
  void server_t<request_container_t, request_info_t>::handle_request(std::list<zmq::message_t>& messages) {
    //must be an identity frame and a message frame if a request larger than
    //zmq::in_batch_size (8192) is sent over a stream socket it will be broken
    //up into multiple messages, however each piece will come with an identity
    //frame so we dont need to worry about there being more than 2 message frames
    if(messages.size() != 2) {
      logging::WARN("Ignoring request: wrong number of parts");
      //TODO: disconnect client?
      return;
    }

    //in versions 4.1 and up ZMQ_STREAM_NOTIFY is defaulted to on which means
    //you will receive connect and disconnect messages on stream sockets
    //for a time this was broken. 4.0 did not send them, then 4.1 did, then 4.2
    //didn't but it was agreed that it would be better if we just kept it on
    //and added an option, ZMQ_STREAM_NOTIFY, to turn it off. this was back ported
    //to 4.1 so that from 4.1 on you'll get these connect and disconnect messages
    //unless you turn it off.

    //get some info about the client
    auto requester = std::move(messages.front());
    auto session = sessions.find(requester);

    //open or close connection
    const auto& body = *std::next(messages.begin());
    if(body.size() == 0) {
      //connect by making space for a streaming request
      if(session == sessions.end()) {
        sessions.emplace(std::move(requester), request_container_t{});
      }//disconnect by interrupting of all the requests
      else {
        for(auto id_time_stamp : session->second.enqueued)
          interrupt.send(static_cast<void*>(&id_time_stamp), sizeof(id_time_stamp), ZMQ_DONTWAIT);
        sessions.erase(session);
      }
    }//actual request data
    else {
      if(session != sessions.end()) {
        //proxy any whole bits onward, or if that failed (malformed or large request) close the session
        if(!enqueue(session->first, body, session->second)) {
          client.send(session->first, ZMQ_SNDMORE | ZMQ_DONTWAIT);
          client.send(static_cast<const void*>(""), 0, ZMQ_DONTWAIT);
          for(auto id_time_stamp : session->second.enqueued)
            interrupt.send(static_cast<void*>(&id_time_stamp), sizeof(id_time_stamp), ZMQ_DONTWAIT);
          sessions.erase(session);
        }
      }
      else
        logging::WARN("Ignoring request: unknown client");
    }
  }

  proxy_t::proxy_t(zmq::context_t& context, const std::string& upstream_endpoint, const std::string& downstream_endpoint, const choose_function_t& choose_function):
    upstream(context, ZMQ_ROUTER), downstream(context, ZMQ_ROUTER), choose_function(choose_function) {

    int disabled = 0;

    upstream.setsockopt(ZMQ_RCVHWM, &disabled, sizeof(disabled));
    upstream.setsockopt(ZMQ_SNDHWM, &disabled, sizeof(disabled));
    upstream.bind(upstream_endpoint.c_str());

    downstream.setsockopt(ZMQ_RCVHWM, &disabled, sizeof(disabled));
    downstream.setsockopt(ZMQ_SNDHWM, &disabled, sizeof(disabled));
    downstream.bind(downstream_endpoint.c_str());
  }
  proxy_t::~proxy_t(){}
  int proxy_t::expire() {
    //TODO: expire any workers who don't advertise for a while, simply store a pair
    //in the heartbeat fifo where the second item is the time it was added. then we
    //just get the time and iterate from the beginning popping off stale ones
    return static_cast<bool>(fifo.size()) + 1;
  }
  void proxy_t::forward() {
    //keep forwarding messages
    while(true) {
      //check for activity on either of the sockets, but if we have no workers just let requests sit on the upstream socket
      zmq::pollitem_t items[] = { { downstream, 0, ZMQ_POLLIN, 0 }, { upstream, 0, ZMQ_POLLIN, 0 } };
      zmq::poll(items, expire(), -1);

      //this worker is bored
      if(items[0].revents & ZMQ_POLLIN) {
        try {
          //its a new worker
          auto messages = downstream.recv_all(ZMQ_DONTWAIT);
          auto worker = workers.find(messages.front());
          if(worker == workers.cend()) {
            //take ownership of heartbeat
            fifo.emplace_back(std::move(*std::next(messages.begin())));
            //remember this workers address
            worker = workers.emplace_hint(worker, std::move(messages.front()), std::prev(fifo.end()));
            //remember which worker owns this heartbeat
            heart_beats.emplace(&fifo.back(), worker->first);
          }//not new but update heartbeat just in case
          else
            *worker->second = std::move(*std::next(messages.begin()));
        }
        catch(const std::exception& e) {
          logging::ERROR(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " proxy_t: " + e.what());
        }
      }

      //request for work
      if(items[1].revents & ZMQ_POLLIN) {
        try {
          //get the request
          auto messages = upstream.recv_all(ZMQ_DONTWAIT);
          //strip the from address (previous hop)
          messages.pop_front();
          //figure out what worker you want, ignore the request info
          auto info = std::move(messages.front());
          messages.pop_front();
          const auto* heart_beat = choose_function(fifo, messages);
          messages.emplace_front(std::move(info));
          //either you didnt want to choose or you sent back garbage
          auto hb_itr = heart_beats.find(heart_beat);
          if(heart_beat == nullptr || hb_itr == heart_beats.cend()) {
            heart_beat = &fifo.front();
            hb_itr = heart_beats.find(heart_beat);
          }
          //send it on to the first bored worker
          downstream.send(hb_itr->second, ZMQ_DONTWAIT | ZMQ_SNDMORE);
          downstream.send_all(messages, ZMQ_DONTWAIT);
          //they are dead to us until they report back
          auto worker_itr = workers.find(hb_itr->second);
          fifo.erase(worker_itr->second);
          workers.erase(worker_itr);
          heart_beats.erase(hb_itr);
        }
        catch (const std::exception& e) {
          //TODO: recover from a worker dying just before you sent it work
          logging::ERROR(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " proxy_t: " + e.what());
        }
      }
    }
  }

  worker_t::worker_t(zmq::context_t& context, const std::string& upstream_proxy_endpoint, const std::string& downstream_proxy_endpoint,
                     const std::string& result_endpoint, const std::string& interrupt_endpoint, const work_function_t& work_function,
                     const cleanup_function_t& cleanup_function, const std::string& heart_beat):
    upstream_proxy(context, ZMQ_DEALER), downstream_proxy(context, ZMQ_DEALER), loopback(context, ZMQ_PUB), interrupt(context, ZMQ_SUB),
    work_function(work_function), cleanup_function(cleanup_function), heart_beat_interval(5000), heart_beat(heart_beat), job(std::numeric_limits<decltype(job)>::max()) {

    int disabled = 0;

    upstream_proxy.setsockopt(ZMQ_RCVHWM, &disabled, sizeof(disabled));
    upstream_proxy.setsockopt(ZMQ_SNDHWM, &disabled, sizeof(disabled));
    upstream_proxy.connect(upstream_proxy_endpoint.c_str());

    downstream_proxy.setsockopt(ZMQ_RCVHWM, &disabled, sizeof(disabled));
    downstream_proxy.setsockopt(ZMQ_SNDHWM, &disabled, sizeof(disabled));
    downstream_proxy.connect(downstream_proxy_endpoint.c_str());

    loopback.setsockopt(ZMQ_SNDHWM, &disabled, sizeof(disabled));
    loopback.connect(result_endpoint.c_str());

    interrupt.setsockopt(ZMQ_RCVHWM, &disabled, sizeof(disabled));
    interrupt.setsockopt(ZMQ_SUBSCRIBE, "", 0);
    interrupt.connect(interrupt_endpoint.c_str());
  }
  worker_t::~worker_t() {}
  void worker_t::work() {
    //give us something to do
    advertise();
    //give client code a way to abort
    interrupt_function_t bail = std::bind(&worker_t::handle_interrupt, this, false);

    //keep forwarding messages
    while(true) {
      //check for activity on the in bound socket, timeout after heart_beat interval
      zmq::pollitem_t items[] = { { upstream_proxy, 0, ZMQ_POLLIN, 0 }, { interrupt, 0, ZMQ_POLLIN, 0 } };
      zmq::poll(items, 2, heart_beat_interval);

      //got some work to do
      if(items[0].revents & ZMQ_POLLIN) {
        try {
          //strip off the request info
          auto messages = upstream_proxy.recv_all(0);
          auto request_info = std::move(messages.front());
          messages.pop_front();
          //check if this request_info is one we should abort
          job = *static_cast<const uint64_t*>(request_info.data());
          handle_interrupt(true);
          //do the work
          auto result = work_function(messages, request_info.data(), bail);
          //we'll keep advertising with this heartbeat
          heart_beat = std::move(result.heart_beat);
          //should we send this on to the next proxy
          if(result.intermediate) {
            downstream_proxy.send(request_info, ZMQ_SNDMORE);
            downstream_proxy.send_all(result.messages, 0);
          }//or are we done
          else {
            loopback.send(request_info, ZMQ_SNDMORE);
            if(result.messages.size() == 0)
              logging::ERROR("At least one result message is required for the loopback");
            if(result.messages.size() > 1) {
              logging::WARN("Sending more than one result message over the loopback will result in additional parts being dropped");
              result.messages.resize(1);
            }
            if(result.messages.back().size() == 0)
              logging::WARN("Sending empty messages will disconnect the client");
            loopback.send_all(result.messages, 0);
          }
        }//either interrupted or something unknown TODO: catch everything to avoid crashing?
        catch(const interrupt_t& i) { logging::WARN(i.what()); }
        catch(const std::exception& e) { logging::ERROR(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " worker_t: " + e.what()); }

        //reset the job
        job = std::numeric_limits<decltype(job)>::max();

        //do some cleanup
        try { cleanup_function(); }
        catch(const std::exception& e) { logging::ERROR(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " worker_t: " + e.what()); }
      }

      //got interrupt(s)
      if(items[1].revents & ZMQ_POLLIN) {
        handle_interrupt(false);
        //TODO: trim the fat while we are waiting for work
      }

      //we want something more to do
      advertise();
    }
  }
  void worker_t::advertise() {
    try {
      //heart beat, we're alive
      upstream_proxy.send(static_cast<const void*>(heart_beat.c_str()), heart_beat.size(), 0);
    }
    catch (const std::exception& e) {
      logging::ERROR(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " worker_t: " + e.what());
    }
  }
  void worker_t::handle_interrupt(bool force_check) {
    //is there anything there right now
    auto messages = interrupt.recv_all(ZMQ_DONTWAIT);
    for(const auto& message : messages)
      interrupts.insert(*static_cast<const decltype(job)*>(message.data()));

    //either we just got more or we need to check the backlog
    if((force_check || messages.size()) && interrupts.find(job) != interrupts.cend())
      throw interrupt_t(job << 32);
  }

  //explicit instantiation for netstring and http
  template class server_t<netstring_entity_t, netstring_request_info_t>;
  template class server_t<http_request_t, http_request_info_t>;


}
