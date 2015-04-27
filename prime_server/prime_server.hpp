#ifndef __PRIME_SERVER_HPP__
#define __PRIME_SERVER_HPP__

#include <zmq.hpp>
#include <functional>
#include <string>
#include <queue>
#include <deque>
#include <list>
#include <unordered_set>
#include <unordered_map>
#include <stdexcept>
#include <random>
#include <memory>
#include <limits>

#include "logging/logging.hpp"

//NOTE: ZMQ_STREAM sockets are 'raw' and require a lot of extra work compared to other types
//one issue is that messages coalesce on the socket (makes sense given the name stream right)
//but there seems to be an internal buffer that, on my machine is around 8192 bytes
//this means that you have to always be careful of partial messages and have some means of
//knowing when a message is whole or not.

namespace {

  //TODO: make this configurable
  constexpr size_t MAX_REQUEST_SIZE = 1024;

  //read all of the messages from a socket
  std::list<zmq::message_t> recv_all(zmq::socket_t& socket) {
    //grab all message parts
    std::list<zmq::message_t> messages;
    int more;
    size_t more_size = sizeof(more);
    do {
      messages.emplace_back();
      socket.recv(&messages.back());
      socket.getsockopt(ZMQ_RCVMORE, &more, &more_size);
    } while(more);
    return messages;
  }

  //send all the messages over this socket
  void send_all(zmq::socket_t& socket, std::list<zmq::message_t>& messages) {
    const auto* last_message = &messages.back();
    for(auto& message : messages)
      socket.send(message, (last_message == &message ? 0 : ZMQ_SNDMORE));
  }

}

namespace prime_server {

  //TODO: do a make_shared for zmq context and socket

  //client makes requests and gets back responses in batches asynchronously
  template <class protocol_type>
  class client_t {
    using request_function_t = std::function<std::pair<const void*, size_t> ()>;
    using collect_function_t = std::function<bool (const std::pair<const void*, size_t>&)>;
   public:
    client_t(const std::shared_ptr<zmq::context_t>& context_ptr, const std::string& server_endpoint, const request_function_t& request_function,
      const collect_function_t& collect_function, size_t batch_size = 8912):
      context_ptr(context_ptr), server(*context_ptr, ZMQ_STREAM), request_function(request_function), collect_function(collect_function), batch_size(batch_size) {

      int disabled = 0;
      server.setsockopt(ZMQ_SNDHWM, &disabled, sizeof(disabled));
      server.setsockopt(ZMQ_RCVHWM, &disabled, sizeof(disabled));
#if ZMQ_VERSION_MAJOR >= 4
#if ZMQ_VERSION_MINOR >= 1
      int enabled = 1;
      server.setsockopt(ZMQ_STREAM_NOTIFY, &enabled, sizeof(enabled));
#endif
#endif
      server.connect(server_endpoint.c_str());
    }
    void batch() {
      //swallow the first response as its just for connecting
      //TODO: make sure it looks right
      recv_all(server);

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
            auto message = protocol_type::delineate(request.first, request.second);
            //send the stuff on
            server.send(static_cast<const void*>(identity), identity_size, ZMQ_SNDMORE);
            server.send(message);
          }
          catch(const std::exception& e) {
            LOG_ERROR(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " client_t: " + e.what());
          }
        }

        //receive some
        std::string response_data;
        current_batch = 0;
        while(more && current_batch < batch_size) {
          try {
            //see if we are still waiting for stuff
            auto messages = recv_all(server);
            messages.pop_front();
            response_data.append(static_cast<const char*>(messages.front().data()), messages.front().size());
            size_t consumed;
            auto responses = protocol_type::separate(response_data.c_str(), response_data.size(), consumed);
            for(const auto& reponse : responses) {
              more = collect_function(reponse);
              ++current_batch;
            }
            response_data.erase(0, consumed);
          }
          catch(const std::exception& e) {
            LOG_ERROR(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " client_t: " + e.what());
          }
        }
      }
    }
   protected:
    std::shared_ptr<zmq::context_t> context_ptr;
    zmq::socket_t server;
    request_function_t request_function;
    collect_function_t collect_function;
    size_t batch_size;
  };

  //server sits between a clients and a load balanced backend
  template <class protocol_type>
  class server_t {
   public:
    server_t(const std::shared_ptr<zmq::context_t>& context_ptr, const std::string& client_endpoint, const std::string& proxy_endpoint, const std::string& result_endpoint):
      context_ptr(context_ptr), client(*context_ptr, ZMQ_STREAM), proxy(*context_ptr, ZMQ_DEALER), loopback(*context_ptr, ZMQ_SUB) {

      int disabled = 0;
      client.setsockopt(ZMQ_SNDHWM, &disabled, sizeof(disabled));
      client.setsockopt(ZMQ_RCVHWM, &disabled, sizeof(disabled));
#if ZMQ_VERSION_MAJOR >= 4
#if ZMQ_VERSION_MINOR >= 1
      int enabled = 1;
      client.setsockopt(ZMQ_STREAM_NOTIFY, &enabled, sizeof(enabled));
#endif
#endif
      client.bind(client_endpoint.c_str());

      proxy.setsockopt(ZMQ_RCVHWM, &disabled, sizeof(disabled));
      proxy.setsockopt(ZMQ_SNDHWM, &disabled, sizeof(disabled));
      proxy.connect(proxy_endpoint.c_str());

      loopback.setsockopt(ZMQ_RCVHWM, &disabled, sizeof(disabled));
      loopback.setsockopt(ZMQ_SUBSCRIBE, "", 0);
      loopback.bind(result_endpoint.c_str());

      requests.reserve(1024);
    }
    void serve() {
      while(true) {
        //check for activity on the client socket and the result socket
        zmq::pollitem_t items[] = { { loopback, 0, ZMQ_POLLIN, 0 }, { client, 0, ZMQ_POLLIN, 0 } };
        zmq::poll(items, 2, -1);

        //got a new result
        if(items[0].revents & ZMQ_POLLIN) {
          try {
            auto messages = recv_all(loopback);
            handle_response(messages);
          }
          catch(const std::exception& e) {
            LOG_ERROR(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " server_t: " + e.what());
          }
        }

        //got a new request
        if(items[1].revents & ZMQ_POLLIN) {
          try {
            auto messages = recv_all(client);
            handle_request(messages);
          }
          catch(const std::exception& e) {
            LOG_ERROR(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " server_t: " + e.what());
          }
        }
      }
    }
   protected:
    void handle_response(std::list<zmq::message_t>& messages) {
      if(messages.size() != 2)
        LOG_WARN("Cannot reply with more than one message, dropping additional");
      client.send(messages.front(), ZMQ_SNDMORE);
      messages.pop_front();
      auto response = protocol_type::delineate(messages.front().data(), messages.front().size());
      client.send(response);
    }
    void handle_request(std::list<zmq::message_t>& messages) {
      //cant be more than 2 messages
      if(messages.size() != 2) {
        LOG_WARN("Ignoring request: too many parts");
        return;
      }

      //get some info about the client
      auto requester = std::string(static_cast<const char*>(messages.front().data()), messages.front().size());
      auto request = requests.find(requester);
      auto& body = *std::next(messages.begin());

      //open or close connection
      if(body.size() == 0) {
        //new client
        if(request == requests.end()) {
          requests.emplace(requester, "");
        }//TODO: check if disconnecting client has a partial request waiting here
        else {
          requests.erase(request);
        }
      }//actual request data
      else {
        if(request != requests.end()) {
          //put this part of the request with the rest
          auto& request_data = request->second;
          request_data.append(static_cast<const char*>(body.data()), body.size());
          //see how many requests we have
          size_t consumed;
          auto separated = protocol_type::separate(request_data.c_str(), request_data.size(), consumed);
          //send them all into the machine
          for(const auto& separate : separated) {
            proxy.send(static_cast<const void*>(requester.c_str()), requester.size(), ZMQ_SNDMORE);
            proxy.send(separate.first, separate.second, 0);
          }
          request_data.erase(0, consumed);

          //hangup if this is all too much
          //TODO: 414 for http clients
          if(request_data.size() > MAX_REQUEST_SIZE) {
            requests.erase(request);
            body.rebuild(0);
            handle_response(messages);
            return;
          }
        }
        else
          LOG_WARN("Ignoring request: unknown client");
      }
    }

    std::shared_ptr<zmq::context_t> context_ptr;
    zmq::socket_t client;
    zmq::socket_t proxy;
    zmq::socket_t loopback;
    //TODO: have a reverse look up by time of connection, kill connections that stick around for a long time
    std::unordered_map<std::string, std::string> requests;
  };

  //proxy messages between layers of a backend load balancing in between
  class proxy_t {
   public:
    proxy_t(const std::shared_ptr<zmq::context_t>& context_ptr, const std::string& upstream_endpoint, const std::string& downstream_endpoint):
      context_ptr(context_ptr), upstream(*context_ptr, ZMQ_ROUTER), downstream(*context_ptr, ZMQ_ROUTER) {

      int disabled = 0;

      upstream.setsockopt(ZMQ_RCVHWM, &disabled, sizeof(disabled));
      upstream.setsockopt(ZMQ_SNDHWM, &disabled, sizeof(disabled));
      upstream.bind(upstream_endpoint.c_str());

      downstream.setsockopt(ZMQ_RCVHWM, &disabled, sizeof(disabled));
      downstream.setsockopt(ZMQ_SNDHWM, &disabled, sizeof(disabled));
      downstream.bind(downstream_endpoint.c_str());
    }
    void forward() {
      std::unordered_set<std::string> workers;
      std::queue<std::string> fifo(std::deque<std::string>{});

      //keep forwarding messages
      while(true) {
        //TODO: expire any workers who don't advertise for a while

        //check for activity on either of the sockets, but if we have no workers just let requests sit on the upstream socket
        zmq::pollitem_t items[] = { { downstream, 0, ZMQ_POLLIN, 0 }, { upstream, 0, ZMQ_POLLIN, 0 } };
        zmq::poll(items,  workers.size() ? 2 : 1, -1);

        //this worker is bored
        if(items[0].revents & ZMQ_POLLIN) {
          try {
            auto messages = recv_all(downstream);
            auto inserted = workers.insert(std::string(static_cast<const char*>(messages.front().data()), messages.front().size()));
            if(inserted.second)
              fifo.push(*inserted.first);
          }
          catch(const std::exception& e) {
            LOG_ERROR(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " proxy_t: " + e.what());
          }
        }

        //request for work
        if(items[1].revents & ZMQ_POLLIN) {
          try {
            //get the request
            auto messages = recv_all(upstream);
            //strip the from address and replace with first bored worker
            messages.pop_front();
            auto worker_address = fifo.front();
            workers.erase(worker_address);
            fifo.pop();
            //send it on to the worker
            downstream.send(static_cast<const void*>(worker_address.c_str()), worker_address.size(), ZMQ_SNDMORE);
            send_all(downstream, messages);
          }
          catch (const std::exception& e) {
            //TODO: recover from a worker dying just before you send it work
            LOG_ERROR(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " proxy_t: " + e.what());
          }
        }
      }
    }
   protected:
    std::shared_ptr<zmq::context_t> context_ptr;
    zmq::socket_t upstream;
    zmq::socket_t downstream;
  };

  //get work from a load balancer proxy letting it know when you are idle
  class worker_t {
   public:
    struct result_t {
      bool intermediate;
      std::list<zmq::message_t> messages;
    };
    using work_function_t = std::function<result_t (const std::list<zmq::message_t>&)>;

    worker_t(const std::shared_ptr<zmq::context_t>& context_ptr, const std::string& upstream_proxy_endpoint, const std::string& downstream_proxy_endpoint,
      const std::string& result_endpoint, const work_function_t& work_function):
      context_ptr(context_ptr), upstream_proxy(*context_ptr, ZMQ_DEALER), downstream_proxy(*context_ptr, ZMQ_DEALER), loopback(*context_ptr, ZMQ_PUB),
      work_function(work_function), heart_beat(5000) {

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
    void work() {
      //give us something to do
      advertise();

      //keep forwarding messages
      while(true) {
        //check for activity on the in bound socket, timeout after heart_beat interval
        zmq::pollitem_t item = { upstream_proxy, 0, ZMQ_POLLIN, 0 };
        zmq::poll(&item, 1, heart_beat);

        //got some work to do
        if(item.revents & ZMQ_POLLIN) {
          try {
            auto messages = recv_all(upstream_proxy);
            auto address = std::move(messages.front());
            messages.pop_front();
            auto result = work_function(messages);
            //should we send this on to the next proxy
            if(result.intermediate) {
              downstream_proxy.send(address, ZMQ_SNDMORE);
              send_all(downstream_proxy, result.messages);
            }//or are we done
            else {
              if(result.messages.size() > 1) {
                while(result.messages.size() > 1)
                  result.messages.pop_back();
                LOG_WARN("Cannot send more than one result message, additional parts are dropped");
              }
              loopback.send(address, ZMQ_SNDMORE);
              send_all(loopback, result.messages);
            }
          }
          catch(const std::exception& e) {
            LOG_ERROR(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " worker_t: " + e.what());
          }
        }

        //we want something more to do
        advertise();
      }
    }
   protected:
    void advertise() {
      try {
        //heart beat, we're alive
        upstream_proxy.send(static_cast<const void*>(""), 0);
      }
      catch (const std::exception& e) {
        LOG_ERROR(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " worker_t: " + e.what());
      }
    }
    std::shared_ptr<zmq::context_t> context_ptr;
    zmq::socket_t upstream_proxy;
    zmq::socket_t downstream_proxy;
    zmq::socket_t loopback;
    work_function_t work_function;
    long heart_beat;
  };

}

#endif //__PRIME_SERVER_HPP__
