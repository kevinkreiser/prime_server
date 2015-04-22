#ifndef __MESSAGING_MESSAGING_HPP__
#define __MESSAGING_MESSAGING_HPP__

#include <zmq.hpp>
#include <functional>
#include <string>
#include <queue>
#include <deque>
#include <list>
#include <stdexcept>
#include <random>
#include <memory>

#include "logging/logging.hpp"

namespace {

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

  //send all the messages over this socket to a specific address
  void send_to(const std::string& address, zmq::socket_t& socket, std::list<zmq::message_t>& messages) {
    socket.send(static_cast<const void*>(address.c_str()), address.size(), ZMQ_SNDMORE);
    send_all(socket, messages);
  }

  //send all the messages over this socket to a specific address
  void send_to(zmq::message_t& address, zmq::socket_t& socket, std::list<zmq::message_t>& messages) {
    socket.send(address, ZMQ_SNDMORE);
    send_all(socket, messages);
  }

  //sets the identity on the client socket
  std::string set_identity(zmq::socket_t& socket) {
    std::random_device device;
    auto identity = std::to_string(device());
    socket.setsockopt(ZMQ_IDENTITY, identity.data(), identity.size());
    return identity;
  }

  void log(const std::list<zmq::message_t>& messages) {
    for(const auto& msg : messages)
      LOG_INFO(std::string(static_cast<const char*>(msg.data()), msg.size()));
  }

}

namespace messaging {

  //client makes requests and gets back responses in batches asynchronously
  class client_t {
    using request_function_t = std::function<std::list<zmq::message_t> ()>;
    using collect_function_t = std::function<bool (const std::list<zmq::message_t>&)>;
   public:
    client_t(const std::shared_ptr<zmq::context_t>& context_ptr, const std::string& server_endpoint, const request_function_t& request_function,
      const collect_function_t& collect_function):
      context_ptr(context_ptr), server(*context_ptr, ZMQ_DEALER), request_function(request_function), collect_function(collect_function) {

      int high_water_mark = 0;
      server.setsockopt(ZMQ_SNDHWM, &high_water_mark, sizeof(high_water_mark));
      server.setsockopt(ZMQ_RCVHWM, &high_water_mark, sizeof(high_water_mark));
      auto identity = set_identity(server);
      server.connect(server_endpoint.c_str());
    }
    void request() {
      while(true) {
        try {
          //see if we are still making stuff
          auto messages = request_function();
          if(messages.size() == 0)
            break;
          //send the stuff on
          LOG_INFO("SEND REQUEST");
          send_all(server, messages);
          LOG_INFO("/SEND REQUEST");
        }
        catch(const std::exception& e) {
          LOG_ERROR(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " client_t: " + e.what());
        }
      }
    }
    void collect() {
      while(true) {
        try {
          //see if we are still waiting for stuff
          auto messages = recv_all(server);
          LOG_INFO("GET RESULT");
          if(collect_function(messages))
            break;
          LOG_INFO("/GET RESULT");
        }
        catch(const std::exception& e) {
          LOG_ERROR(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " client_t: " + e.what());
        }
      }
    }
   protected:
    std::shared_ptr<zmq::context_t> context_ptr;
    zmq::socket_t server;
    request_function_t request_function;
    collect_function_t collect_function;
  };

  //server sits between a clients and a load balanced backend
  class server_t {
   public:
    server_t(const std::shared_ptr<zmq::context_t>& context_ptr, const std::string& client_endpoint, const std::string& proxy_endpoint, const std::string& result_endpoint):
      context_ptr(context_ptr), client(*context_ptr, ZMQ_ROUTER), proxy(*context_ptr, ZMQ_DEALER), loopback(*context_ptr, ZMQ_SUB) {

      //int raw_router = 1;
      //client.setsockopt(ZMQ_ROUTER_RAW, &raw_router, sizeof(raw_router));
      int high_water_mark = 0;
      client.setsockopt(ZMQ_SNDHWM, &high_water_mark, sizeof(high_water_mark));
      client.setsockopt(ZMQ_RCVHWM, &high_water_mark, sizeof(high_water_mark));
      client.bind(client_endpoint.c_str());

      proxy.setsockopt(ZMQ_RCVHWM, &high_water_mark, sizeof(high_water_mark));
      proxy.setsockopt(ZMQ_SNDHWM, &high_water_mark, sizeof(high_water_mark));
      auto identity = set_identity(proxy);
      proxy.connect(proxy_endpoint.c_str());

      loopback.setsockopt(ZMQ_RCVHWM, &high_water_mark, sizeof(high_water_mark));
      loopback.setsockopt(ZMQ_SUBSCRIBE, "", 0);
      loopback.bind(result_endpoint.c_str());
    }
    void serve() {
      while(true) {
        //check for activity on the client socket and the result socket
        zmq::pollitem_t items[] = { { client, 0, ZMQ_POLLIN, 0 }, { loopback, 0, ZMQ_POLLIN, 0 } };
        zmq::poll(items, 2, -1);

        //got a new request
        if(items[0].revents & ZMQ_POLLIN) {
          try {
            LOG_INFO("NEW REQUEST");
            auto messages = recv_all(client);
            log(messages);
            LOG_INFO("/NEW REQUEST");
            LOG_INFO("FORWARD REQUEST");
            send_all(proxy, messages);
            LOG_INFO("/FORWARD REQUEST");
          }
          catch(const std::exception& e) {
            LOG_ERROR(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " server_t: " + e.what());
          }
        }

        //got a new result
        if(items[1].revents & ZMQ_POLLIN) {
          try {
            LOG_INFO("GET RESULT");
            auto messages = recv_all(loopback);
            LOG_INFO("/GET RESULT");
            LOG_INFO("FORWARD RESULT");
            send_all(client, messages);
            LOG_INFO("/FORWARD RESULT");
          }
          catch(const std::exception& e) {
            LOG_ERROR(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " server_t: " + e.what());
          }
        }
      }
    }
   protected:
    std::shared_ptr<zmq::context_t> context_ptr;
    zmq::socket_t client;
    zmq::socket_t proxy;
    zmq::socket_t loopback;
  };

  //proxy messages between layers of a backend load balancing in between
  class proxy_t {
   public:
    proxy_t(const std::shared_ptr<zmq::context_t>& context_ptr, const std::string& upstream_endpoint, const std::string& downstream_endpoint):
      context_ptr(context_ptr), upstream(*context_ptr, ZMQ_ROUTER), downstream(*context_ptr, ZMQ_ROUTER) {

      int high_water_mark = 0;

      upstream.setsockopt(ZMQ_RCVHWM, &high_water_mark, sizeof(high_water_mark));
      upstream.setsockopt(ZMQ_SNDHWM, &high_water_mark, sizeof(high_water_mark));
      upstream.bind(upstream_endpoint.c_str());

      downstream.setsockopt(ZMQ_RCVHWM, &high_water_mark, sizeof(high_water_mark));
      downstream.setsockopt(ZMQ_SNDHWM, &high_water_mark, sizeof(high_water_mark));
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
            LOG_INFO("PROXY REQUEST IN");
            auto messages = recv_all(upstream);
            log(messages);
            LOG_INFO("/PROXY REQUEST IN");
            //strip the from address and replace with first bored worker
            messages.erase(messages.begin());
            auto worker_address = fifo.front();
            workers.erase(worker_address);
            fifo.pop();
            //send it on to the worker
            LOG_INFO("PROXY REQUEST OUT");
            send_to(worker_address, downstream, messages);
            LOG_INFO("/PROXY REQUEST OUT");
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

      int high_water_mark = 0;

      upstream_proxy.setsockopt(ZMQ_RCVHWM, &high_water_mark, sizeof(high_water_mark));
      upstream_proxy.setsockopt(ZMQ_SNDHWM, &high_water_mark, sizeof(high_water_mark));
      auto identity_up = set_identity(upstream_proxy);
      upstream_proxy.connect(upstream_proxy_endpoint.c_str());

      downstream_proxy.setsockopt(ZMQ_RCVHWM, &high_water_mark, sizeof(high_water_mark));
      downstream_proxy.setsockopt(ZMQ_SNDHWM, &high_water_mark, sizeof(high_water_mark));
      auto identity_down = set_identity(downstream_proxy);
      downstream_proxy.connect(downstream_proxy_endpoint.c_str());

      loopback.setsockopt(ZMQ_SNDHWM, &high_water_mark, sizeof(high_water_mark));
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
            LOG_INFO("GET JOB");
            auto messages = recv_all(upstream_proxy);
            auto address = std::move(messages.front());
            messages.erase(messages.begin());
            log(messages);
            LOG_INFO("/GET JOB");
            auto result = work_function(messages);
            //should we send this on to the next proxy
            if(result.intermediate) {
              LOG_INFO("PASS ON");
              send_to(address, downstream_proxy, result.messages);
              LOG_INFO("/PASS ON");
            }//or are we done
            else {
              LOG_INFO("SEND RESULT");
              send_to(address, loopback, result.messages);
              LOG_INFO("/SEND RESULT");
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

#endif //__MESSAGING_MESSAGING_HPP__
