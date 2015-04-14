#ifndef __MESSAGING_MESSAGING_HPP__
#define __MESSAGING_MESSAGING_HPP__

#include <zmq.hpp>
#include <functional>
#include <memory>
#include <string>
#include <queue>
#include <deque>
#include <list>

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

  //client makes requests and gets back responses in batches asynchronously
  class client {
    using request_function_t = std::function<std::list<zmq::message_t> ()>;
    using collect_function_t = std::function<bool (const std::list<zmq::message_t>&)>;
   public:
    client(const std::shared_ptr<zmq::context_t>& context, const char* server_endpoint, const request_function_t& request_function,
      const collect_function_t& collect_function):
      context(context), server_socket(*this->context, ZMQ_DEALER), request_function(request_function), collect_function(collect_function) {

      int high_water_mark = 0;
      server_socket.setsockopt(ZMQ_SNDHWM, &high_water_mark, sizeof(high_water_mark));
      server_socket.setsockopt(ZMQ_RCVHWM, &high_water_mark, sizeof(high_water_mark));
      server_socket.connect(server_endpoint);
    }
    void request() {
      while(true) {
        //see if we are still making stuff
        auto messages = request_function();
        if(messages.size() == 0)
          break;
        //send the stuff on
        const auto* last_message = &messages.back();
        for(auto& message : messages)
          server_socket.send(message, (last_message == &message ? 0 : ZMQ_SNDMORE) | ZMQ_NOBLOCK);
      }
    }
    void collect() {
      while(true) {
        //see if we are still waiting for stuff
        auto messages = recv_all(server_socket);
        if(collect_function(messages))
          break;
      }
    }
   protected:
    std::shared_ptr<zmq::context_t> context;
    zmq::socket_t server_socket;
    request_function_t request_function;
    collect_function_t collect_function;
  };

  //server sits between a clients and a load balanced backend
  class server {
    using result_function_t = std::function<void (const std::list<zmq::message_t>&, bool)>;
   public:
    server(const std::shared_ptr<zmq::context_t>& context, const char* client_endpoint, const char* lb_endpoint, const char* result_endpoint,
      const result_function_t& result_function):
      context(context), client_socket(*this->context, ZMQ_ROUTER), lb_socket(*this->context, ZMQ_PUSH), result_socket(*this->context, ZMQ_SUB),
      result_function(result_function) {

      int high_water_mark = 0;
      int raw_router = 1;
      //listen for and respond to client requests
      client_socket.setsockopt(ZMQ_SNDHWM, &high_water_mark, sizeof(high_water_mark));
      client_socket.setsockopt(ZMQ_RCVHWM, &high_water_mark, sizeof(high_water_mark));
      client_socket.setsockopt(ZMQ_ROUTER_RAW, &raw_router, sizeof(raw_router));
      client_socket.bind(client_endpoint);
      //shove work into the backend over this load balancer
      lb_socket.setsockopt(ZMQ_SNDHWM, &high_water_mark, sizeof(high_water_mark));
      lb_socket.connect(lb_endpoint);
      //get back 'E'rrors or 'S'uccesses over this subscription
      result_socket.setsockopt(ZMQ_RCVHWM, &high_water_mark, sizeof(high_water_mark));
      result_socket.setsockopt(ZMQ_SUBSCRIBE, static_cast<const void*>("S"), static_cast<size_t>(1));
      result_socket.setsockopt(ZMQ_SUBSCRIBE, static_cast<const void*>("E"), static_cast<size_t>(1));
      result_socket.bind(result_endpoint);
    }
    void serve() {
      while(true) {
        //check for activity on the in bound socket, timeout after heart_beat interval
        zmq::pollitem_t items[] = { { client_socket, 0, ZMQ_POLLIN, 0 }, { result_socket, 0, ZMQ_POLLIN, 0 } };
        zmq::poll(items, 2, -1);

        //got a new request
        if(items[0].revents & ZMQ_POLLIN) {
          try {
            //let someone else worry about these
            auto messages = recv_all(client_socket);
            send_all(lb_socket, messages);
          }
          catch(...) {
            //TODO:
          }
        }

        //got a new result
        if(items[1].revents & ZMQ_POLLIN) {
          try {
            auto messages = recv_all(result_socket);
            bool failure = messages.front() == "E";
            messages.pop_front();
            auto identity = messages.front();
            messages.pop_front();
            result_function(messages, failure);
            messages.push_front({0});
            messages.push_front(identity);
            send_all(client_socket, messages);
          }
          catch(...) {
            //TODO:
          }
        }
      }
    }
   protected:
    std::shared_ptr<zmq::context_t> context;
    zmq::socket_t client_socket;
    zmq::socket_t lb_socket;
    zmq::socket_t result_socket;
    result_function_t result_function;
  };

  //proxy messages between layers of a backend load balancing in between
  class load_balancer {
   public:
    load_balancer(const std::shared_ptr<zmq::context_t>& context, const char* in_endpoint, const char* out_endpoint):
      context(context), in_socket(*this->context, ZMQ_ROUTER), out_socket(*this->context, ZMQ_ROUTER) {

      int high_water_mark = 0;
      in_socket.setsockopt(ZMQ_RCVHWM, &high_water_mark, sizeof(high_water_mark));
      in_socket.setsockopt(ZMQ_SNDHWM, &high_water_mark, sizeof(high_water_mark));
      in_socket.bind(in_endpoint);
      out_socket.setsockopt(ZMQ_RCVHWM, &high_water_mark, sizeof(high_water_mark));
      out_socket.setsockopt(ZMQ_SNDHWM, &high_water_mark, sizeof(high_water_mark));
      out_socket.bind(out_endpoint);
    }
    void balance() {
      //who is available to do work
      //TODO: expire workers who don't heartbeat for a while
      std::unordered_set<std::string> workers;
      std::queue<std::string> fifo(std::deque<std::string>{});
      //keep forwarding messages
      while(true) {
        //check for activity on either of the sockets, but if we have no workers just let requests sit on the in_socket
        zmq::pollitem_t items[] = { { out_socket, 0, ZMQ_POLLIN, 0 }, { in_socket, 0, ZMQ_POLLIN, 0 } };
        zmq::poll(&items[0], (workers.size() ? 2 : 1));

        //this worker is bored
        if(items[0].revents & ZMQ_POLLIN) {
          auto inserted = workers.insert(recv_string(out_socket));
          assert(recv_string(out_socket).size() == 0);
          if(inserted.second)
            fifo.push(*inserted.first);
        }

        //new client request
        if(items[1].revents & ZMQ_POLLIN) {
          //get the request
          auto messages = recv_all(in_socket);
          try {
            //route to the first bored worker
            auto worker_address = fifo.front();
            workers.erase(worker_address);
            fifo.pop();
            out_socket.send(static_cast<const void*>(worker_address.data()), worker_address.size(), ZMQ_SNDMORE | ZMQ_NOBLOCK);
            out_socket.send(static_cast<const void*>(""), 0, ZMQ_SNDMORE | ZMQ_NOBLOCK);
            //all the parts too
            const auto* last_message = &messages.back();
            for(auto& message : messages)
              out_socket.send(message, (last_message == &message ? 0 : ZMQ_SNDMORE) | ZMQ_NOBLOCK);
          }
          catch (...) {
            //TODO: recover from a worker dying
            //temporarily could signal this back over pub socket
          }
        }
      }
    }
   protected:
    std::shared_ptr<zmq::context_t> context;
    zmq::socket_t in_socket;
    zmq::socket_t out_socket;
  };


  //get work from a load balancer proxy letting it know when you are idle
  class worker {
    using work_function_t = std::function<std::list<zmq::message_t> (const std::list<zmq::message_t>&)>;
   public:
    worker(const std::shared_ptr<zmq::context_t>& context, const char* in_endpoint, const char* out_endpoint, const work_function_t& work_function):
      context(context), in_socket(*this->context, ZMQ_PULL), out_socket(*this->context, ZMQ_PUSH), work_function(work_function), heart_beat(5000) {

      int high_water_mark = 0;
      in_socket.setsockopt(ZMQ_RCVHWM, &high_water_mark, sizeof(high_water_mark));
      in_socket.connect(in_endpoint);
      out_socket.setsockopt(ZMQ_SNDHWM, &high_water_mark, sizeof(high_water_mark));
      out_socket.connect(out_endpoint);

      //TODO: add a pub to throw back either results or errors on
    }
    void work() {
      //keep forwarding messages
      while(true) {
        //check for activity on the in bound socket, timeout after heart_beat interval
        zmq::pollitem_t item = { in_socket, 0, ZMQ_POLLIN, 0 };
        zmq::poll(&item, 1, heart_beat);

        //got some work to do
        if(item.revents & ZMQ_POLLIN) {
          try {
            auto messages = recv_all(in_socket);
            auto results = work_function(messages);
            send_all(out_socket, messages);
          }
          catch(...) {
            //TODO:
          }
        }

        try {
          //heart beat, we're alive
          in_socket.send(static_cast<const void*>("HEARTBEAT"), 9, ZMQ_NOBLOCK);
        }
        catch (...) {
          //TODO:
        }
      }
    }
   protected:
    std::shared_ptr<zmq::context_t> context;
    zmq::socket_t in_socket;
    zmq::socket_t out_socket;
    work_function_t work_function;
    long heart_beat;
  };

}

#endif //__MESSAGING_MESSAGING_HPP__
