#ifndef __MESSAGING_MESSAGING_HPP__
#define __MESSAGING_MESSAGING_HPP__

#include <zmq.hpp>
#include <functional>
#include <memory>
#include <string>
#include <queue>
#include <deque>
#include <list>
#include <stdexcept>

namespace messaging {

  //read one message from the socket in the form of a string
  std::string recv_string(zmq::socket_t& socket) {
    zmq::message_t message;
    socket.recv(&message);
    return std::string(static_cast<const char*>(message.data()), message.size());
  }

  //read all of the messages from a socket
  std::list<zmq::message_t> recv_all(zmq::socket_t& socket) {
    //grab all message parts
    std::list<zmq::message_t> messages;
    uint64_t more;
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
      socket.send(message, (last_message == &message ? 0 : ZMQ_SNDMORE) | ZMQ_NOBLOCK);
  }

  //send all the messages over this socket with an identity
  void send_all(zmq::socket_t& socket, std::list<zmq::message_t>& messages,  zmq::message_t& identity) {
    socket.send(static_cast<const void*>(identity.data()), identity.size(), ZMQ_SNDMORE | ZMQ_NOBLOCK);
    socket.send(static_cast<const void*>(""), 0, ZMQ_SNDMORE | ZMQ_NOBLOCK);
    send_all(socket, messages);
  }

  //send all the messages over this socket with an identity
  void send_all(zmq::socket_t& socket, std::list<zmq::message_t>& messages, const std::string& identity) {
    socket.send(static_cast<const void*>(identity.data()), identity.size(), ZMQ_SNDMORE | ZMQ_NOBLOCK);
    socket.send(static_cast<const void*>(""), 0, ZMQ_SNDMORE | ZMQ_NOBLOCK);
    send_all(socket, messages);
  }

  //client makes requests and gets back responses in batches asynchronously
  class client_t {
    using request_function_t = std::function<std::list<zmq::message_t> ()>;
    using collect_function_t = std::function<bool (const std::list<zmq::message_t>&)>;
   public:
    client_t(const std::shared_ptr<zmq::context_t>& context, const std::string& server_endpoint, const request_function_t& request_function,
      const collect_function_t& collect_function):
      context(context), server(*this->context, ZMQ_DEALER), request_function(request_function), collect_function(collect_function) {

      int high_water_mark = 0;
      server.setsockopt(ZMQ_SNDHWM, &high_water_mark, sizeof(high_water_mark));
      server.setsockopt(ZMQ_RCVHWM, &high_water_mark, sizeof(high_water_mark));
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
          send_all(server, messages);
        }
        catch(...) {
          //TODO:
        }
      }
    }
    void collect() {
      while(true) {
        try {
          //see if we are still waiting for stuff
          auto messages = recv_all(server);
          if(collect_function(messages))
            break;
        }
        catch(...) {
          //TODO:
        }
      }
    }
   protected:
    std::shared_ptr<zmq::context_t> context;
    zmq::socket_t server;
    request_function_t request_function;
    collect_function_t collect_function;
  };

  //server sits between a clients and a load balanced backend
  class server_t {
   public:
    server_t(const std::shared_ptr<zmq::context_t>& context, const std::string& client_endpoint, const std::string& proxy_endpoint, const std::string& result_endpoint):
      context(context), client(*this->context, ZMQ_ROUTER), proxy(*this->context, ZMQ_PUSH), loopback(*this->context, ZMQ_PULL) {

      int high_water_mark = 0;
      int raw_router = 1;
      //listen for and respond to client requests
      client.setsockopt(ZMQ_SNDHWM, &high_water_mark, sizeof(high_water_mark));
      client.setsockopt(ZMQ_RCVHWM, &high_water_mark, sizeof(high_water_mark));
      client.setsockopt(ZMQ_ROUTER_RAW, &raw_router, sizeof(raw_router));
      client.bind(client_endpoint.c_str());
      //shove work into the backend over this load balancer
      proxy.setsockopt(ZMQ_SNDHWM, &high_water_mark, sizeof(high_water_mark));
      proxy.connect(proxy_endpoint.c_str());
      //get back results over this subscription
      loopback.setsockopt(ZMQ_RCVHWM, &high_water_mark, sizeof(high_water_mark));
      loopback.bind(result_endpoint.c_str());
    }
    void serve() {
      while(true) {
        //check for activity on the in bound socket, timeout after heart_beat interval
        zmq::pollitem_t items[] = { { client, 0, ZMQ_POLLIN, 0 }, { loopback, 0, ZMQ_POLLIN, 0 } };
        zmq::poll(items, 2, -1);

        //got a new request
        if(items[0].revents & ZMQ_POLLIN) {
          try {
            //let someone else worry about these
            auto messages = recv_all(client);
            send_all(proxy, messages);
          }
          catch(...) {
            //TODO:
          }
        }

        //got a new result
        if(items[1].revents & ZMQ_POLLIN) {
          try {
            auto messages = recv_all(loopback);
            send_all(client, messages);
          }
          catch(...) {
            //TODO:
          }
        }
      }
    }
   protected:
    std::shared_ptr<zmq::context_t> context;
    zmq::socket_t client;
    zmq::socket_t proxy;
    zmq::socket_t loopback;
  };

  //proxy messages between layers of a backend load balancing in between
  class proxy_t {
   public:
    proxy_t(const std::shared_ptr<zmq::context_t>& context, const std::string& upstream_endpoint, const std::string& downstream_endpoint):
      context(context), upstream(*this->context, ZMQ_ROUTER), downstream(*this->context, ZMQ_ROUTER) {

      int high_water_mark = 0;
      int raw_router = 1;
      upstream.setsockopt(ZMQ_RCVHWM, &high_water_mark, sizeof(high_water_mark));
      upstream.setsockopt(ZMQ_SNDHWM, &high_water_mark, sizeof(high_water_mark));
      upstream.setsockopt(ZMQ_ROUTER_RAW, &raw_router, sizeof(raw_router));
      upstream.bind(upstream_endpoint.c_str());
      downstream.setsockopt(ZMQ_RCVHWM, &high_water_mark, sizeof(high_water_mark));
      downstream.setsockopt(ZMQ_SNDHWM, &high_water_mark, sizeof(high_water_mark));
      downstream.setsockopt(ZMQ_ROUTER_RAW, &raw_router, sizeof(raw_router));
      downstream.bind(downstream_endpoint.c_str());
    }
    void forward() {
      //who is available to do work
      std::unordered_set<std::string> workers;
      std::queue<std::string> fifo(std::deque<std::string>{});
      //keep forwarding messages
      while(true) {
        //TODO: expire any workers who don't heartbeat for a while

        //check for activity on either of the sockets, but if we have no workers just let requests sit on the upstream socket
        zmq::pollitem_t items[] = { { downstream, 0, ZMQ_POLLIN, 0 }, { upstream, 0, ZMQ_POLLIN, 0 } };
        zmq::poll(&items[0], (workers.size() ? 2 : 1));

        //this worker is bored
        if(items[0].revents & ZMQ_POLLIN) {
          try {
            auto inserted = workers.insert(recv_string(downstream));
            assert(recv_string(downstream).size() == 0);
            if(inserted.second)
              fifo.push(*inserted.first);
          }
          catch(...) {
            //TODO:
          }
        }

        //request for work
        if(items[1].revents & ZMQ_POLLIN) {
          try {
            //get the request
            auto messages = recv_all(upstream);
            //route to the first bored worker
            auto worker_address = fifo.front();
            workers.erase(worker_address);
            fifo.pop();
            //send it on to the worker
            send_all(downstream, messages, worker_address);
          }
          catch (...) {
            //TODO: recover from a worker dying just before you send it work
          }
        }
      }
    }
   protected:
    std::shared_ptr<zmq::context_t> context;
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

    worker_t(const std::shared_ptr<zmq::context_t>& context, const std::string& upstream_proxy_endpoint, const std::string& downstream_proxy_endpoint,
      const std::string& result_endpoint, const work_function_t& work_function):
      context(context), upstream_proxy(*this->context, ZMQ_PULL), downstream_proxy(*this->context, ZMQ_PUSH),
      loopback(*this->context, ZMQ_PUSH), work_function(work_function), heart_beat(5000) {

      int high_water_mark = 0;
      upstream_proxy.setsockopt(ZMQ_RCVHWM, &high_water_mark, sizeof(high_water_mark));
      upstream_proxy.connect(upstream_proxy_endpoint.c_str());
      downstream_proxy.setsockopt(ZMQ_SNDHWM, &high_water_mark, sizeof(high_water_mark));
      downstream_proxy.connect(downstream_proxy_endpoint.c_str());
      loopback.setsockopt(ZMQ_SNDHWM, &high_water_mark, sizeof(high_water_mark));
      loopback.connect(result_endpoint.c_str());
    }
    void work() {
      //keep forwarding messages
      while(true) {
        //check for activity on the in bound socket, timeout after heart_beat interval
        zmq::pollitem_t item = { upstream_proxy, 0, ZMQ_POLLIN, 0 };
        zmq::poll(&item, 1, heart_beat);

        //got some work to do
        if(item.revents & ZMQ_POLLIN) {
          try {
            auto messages = recv_all(upstream_proxy);
            auto result = work_function(messages);
            //should we send this on to the next proxy or are we done
            send_all(result.intermediate ? downstream_proxy : loopback, result.messages, messages.front());
          }
          catch(...) {
            //TODO:
          }
        }

        try {
          //heart beat, we're alive
          upstream_proxy.send(static_cast<const void*>("HEARTBEAT"), 9, ZMQ_NOBLOCK);
        }
        catch (...) {
          //TODO:
        }
      }
    }
   protected:
    std::shared_ptr<zmq::context_t> context;
    zmq::socket_t upstream_proxy;
    zmq::socket_t downstream_proxy;
    zmq::socket_t loopback;
    work_function_t work_function;
    long heart_beat;
  };

}

#endif //__MESSAGING_MESSAGING_HPP__
