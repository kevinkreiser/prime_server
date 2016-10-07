#include <queue>
#include <deque>
#include <unordered_set>
#include <stdexcept>

#include "prime_server.hpp"
#include "netstring_protocol.hpp"
#include "http_protocol.hpp"
#include "logging.hpp"

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
#if ZMQ_VERSION_MAJOR >= 4
#if ZMQ_VERSION_MINOR >= 1
    //swallow the first response as its just for connecting
    //TODO: make sure it looks right
    server.recv_all(0);
#endif
#endif

    //need the identity to identify our connection when we send stuff
    uint8_t identity[256];
    size_t identity_size = sizeof(identity);
    server.getsockopt(ZMQ_IDENTITY, identity, &identity_size);

    //keep going while we expect more results
    bool more = true;
    while(more) {

      //request some
      size_t current_batch;
      for(current_batch = 0; current_batch < batch_size; ++current_batch) {
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
          LOG_ERROR(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " client_t: " + e.what());
        }
      }

      //receive some
      current_batch = 0;
      while(more && current_batch < batch_size) {
        try {
          //see if we are still waiting for stuff
          auto messages = server.recv_all(0);
          messages.pop_front();
          current_batch += stream_responses(messages.front().data(), messages.front().size(), more);
        }
        catch(const std::exception& e) {
          LOG_ERROR(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " client_t: " + e.what());
        }
      }
    }
  }

  template <class request_container_t, class request_info_t>
  server_t<request_container_t, request_info_t>::server_t(zmq::context_t& context, const std::string& client_endpoint, const std::string& proxy_endpoint,
    const std::string& result_endpoint, bool log, size_t max_request_size): client(context, ZMQ_STREAM), proxy(context, ZMQ_DEALER), loopback(context, ZMQ_SUB),
    log(log), max_request_size(max_request_size) {

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
          handle_response(messages);
        }
        catch(const std::exception& e) {
          LOG_ERROR(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " server_t: " + e.what());
        }
      }

      //got a new request
      if(items[1].revents & ZMQ_POLLIN) {
        try {
          auto messages = client.recv_all(ZMQ_DONTWAIT);
          handle_request(messages);
        }
        catch(const std::exception& e) {
          LOG_ERROR(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " server_t: " + e.what());
        }
      }

      //TODO: kill stale sessions
    }
  }

  template <class request_container_t, class request_info_t>
  void server_t<request_container_t, request_info_t>::handle_response(std::list<zmq::message_t>& messages) {
    if(messages.size() < 2) {
      LOG_ERROR("Cannot reply without request information");
      return;
    }
    if(messages.size() > 2) {
      LOG_WARN("Cannot reply with more than one message, dropping additional");
      messages.resize(2);
    }
    if(messages.back().size() == 0)
      LOG_WARN("Sending empty messages will disconnect the client");

    //reply to client and cleanup request or session
    dequeue(messages);
  }

  template <class request_container_t, class request_info_t>
  void server_t<request_container_t, request_info_t>::handle_request(std::list<zmq::message_t>& messages) {
    //must be an identity frame and a message frame if a request larger than
    //zmq::in_batch_size (8192) is sent over a stream socket it will be broken
    //up into multiple messages, however each piece will come with an identity
    //frame so we dont need to worry about there being more than 2 message frames
    if(messages.size() != 2) {
      LOG_WARN("Ignoring request: wrong number of parts");
      //TODO: disconnect client?
      return;
    }

    //get some info about the client
    auto requester = std::move(messages.front());
    auto session = sessions.find(requester);
#if ZMQ_VERSION_MAJOR <= 4
#if ZMQ_VERSION_MINOR < 1
    //version 4.0 of stream didn't seem to send a blank connection message
    //then they did in 4.1: http://zeromq.org/docs:4-1-upgrade
    //but now they don't again in 4.2 unless you tell them to with NOTIFY
    //and even more complicated stuff happened which got back ported for
    //the sake of consistency. in any case watch out for this
    if(session == sessions.end())
      session = sessions.insert({std::move(requester), request_container_t{}}).first;
#endif
#endif

    //open or close connection
    const auto& body = *std::next(messages.begin());
    if(body.size() == 0) {
      //new client
      if(session == sessions.end())
        sessions.emplace(std::move(requester), request_container_t{});
      //TODO: check if disconnecting client has a partial request waiting here
      else
        sessions.erase(session);
    }//actual request data
    else {
      if(session != sessions.end()) {
        //proxy any whole bits onward, or if that failed (malformed or large request) close the session
        if(!enqueue(session->first, body, session->second)) {
          client.send(session->first, ZMQ_SNDMORE | ZMQ_DONTWAIT);
          client.send(static_cast<const void*>(""), 0, ZMQ_DONTWAIT);
          sessions.erase(session);
        }
      }
      else
        LOG_WARN("Ignoring request: unknown client");
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
  void proxy_t::forward() {
    //we want a fifo queue in the case that the proxy doesnt care what worker to send jobs to
    //having this constraint does also require that we store a bidirectional mapping between
    //worker addresses and their heartbeats. since heartbeats are application defined we only
    //store them once (they could be larger) and opt for storing the worker addresses duplicated
    std::list<zmq::message_t> fifo;
    std::unordered_map<zmq::message_t, std::list<zmq::message_t>::iterator> workers;
    std::unordered_map<const zmq::message_t*, zmq::message_t> heart_beats;

    //keep forwarding messages
    while(true) {
      //TODO: expire any workers who don't advertise for a while

      //check for activity on either of the sockets, but if we have no workers just let requests sit on the upstream socket
      zmq::pollitem_t items[] = { { downstream, 0, ZMQ_POLLIN, 0 }, { upstream, 0, ZMQ_POLLIN, 0 } };
      zmq::poll(items,  fifo.size() ? 2 : 1, -1);

      //this worker is bored
      if(items[0].revents & ZMQ_POLLIN) {
        try {
          auto messages = downstream.recv_all(ZMQ_DONTWAIT);
          auto worker = workers.find(messages.front());
          if(worker == workers.cend()) {
            //take ownership of heartbeat
            fifo.emplace_back(std::move(*std::next(messages.begin())));
            //remember this workers address
            worker = workers.emplace_hint(worker, std::move(messages.front()), std::prev(fifo.end()));
            //remember which worker owns this heartbeat
            heart_beats.emplace(&fifo.back(), worker->first);
          }//TODO: worry about heartbeats updating?
        }
        catch(const std::exception& e) {
          LOG_ERROR(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " proxy_t: " + e.what());
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
          LOG_ERROR(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " proxy_t: " + e.what());
        }
      }
    }
  }

  worker_t::worker_t(zmq::context_t& context, const std::string& upstream_proxy_endpoint, const std::string& downstream_proxy_endpoint,
    const std::string& result_endpoint, const work_function_t& work_function, const cleanup_function_t& cleanup_function, const std::string& heart_beat):
    upstream_proxy(context, ZMQ_DEALER), downstream_proxy(context, ZMQ_DEALER), loopback(context, ZMQ_PUB),
    work_function(work_function), cleanup_function(cleanup_function), heart_beat_interval(5000), heart_beat(heart_beat) {

    int disabled = 0;

    upstream_proxy.setsockopt(ZMQ_RCVHWM, &disabled, sizeof(disabled));
    upstream_proxy.setsockopt(ZMQ_SNDHWM, &disabled, sizeof(disabled));
    upstream_proxy.connect(upstream_proxy_endpoint.c_str());

    downstream_proxy.setsockopt(ZMQ_RCVHWM, &disabled, sizeof(disabled));
    downstream_proxy.setsockopt(ZMQ_SNDHWM, &disabled, sizeof(disabled));
    downstream_proxy.connect(downstream_proxy_endpoint.c_str());

    loopback.setsockopt(ZMQ_SNDHWM, &disabled, sizeof(disabled));
    loopback.connect(result_endpoint.c_str());
  }
  void worker_t::work() {
    //give us something to do
    advertise();

    //keep forwarding messages
    while(true) {
      //check for activity on the in bound socket, timeout after heart_beat interval
      zmq::pollitem_t item = { upstream_proxy, 0, ZMQ_POLLIN, 0 };
      zmq::poll(&item, 1, heart_beat_interval);

      //got some work to do
      if(item.revents & ZMQ_POLLIN) {
        try {
          //strip off the request info
          auto messages = upstream_proxy.recv_all(0);
          auto request_info = std::move(messages.front());
          messages.pop_front();
          //do the work
          auto result = work_function(messages, request_info.data());
          //we'll keep advertising with this heartbeat
          heart_beat = std::move(result.heart_beat);
          //should we send this on to the next proxy
          if(result.intermediate) {
            downstream_proxy.send(request_info, ZMQ_SNDMORE);
            downstream_proxy.send_all(result.messages, 0);
          }//or are we done
          else {
            if(result.messages.size() > 1)
              LOG_WARN("Cannot send more than one result message, additional parts are dropped");
            loopback.send(request_info, ZMQ_SNDMORE);
            loopback.send_all(result.messages, 0);
          }
          //cleanup
          cleanup_function();
        }
        catch(const std::exception& e) {
          LOG_ERROR(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " worker_t: " + e.what());
        }
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
      LOG_ERROR(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " worker_t: " + e.what());
    }
  }

  //explicit instantiation for netstring and http
  template class server_t<netstring_entity_t, uint64_t>;
  template class server_t<http_request_t, http_request_t::info_t>;


}
