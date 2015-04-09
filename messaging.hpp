#ifndef __MESSAGING_MESSAGING_HPP__
#define __MESSAGING_MESSAGING_HPP__

#include <zmq.hpp>
#include <functional>
#include <memory>
#include <string>
#include <list>

namespace messaging {

  //produce messages to a group of workers
  class producer {
    using produce_function_t = std::function<std::list<zmq::message_t> ()>;
   public:
    producer(const std::shared_ptr<zmq::context_t>& context, const char* push_endpoint, const produce_function_t& produce_function):
      context(context), push_socket(*this->context, ZMQ_PUSH), produce_function(produce_function) {

      int high_water_mark = 0;
      push_socket.setsockopt(ZMQ_SNDHWM, &high_water_mark, sizeof(high_water_mark));
      push_socket.bind(push_endpoint);
    }
    void produce() {
      while(true) {
        //see if we are still making stuff
        auto messages = produce_function();
        if(messages.size() == 0)
          break;
        //send the stuff on
        const auto* last_message = &messages.back();
        for(auto& message : messages)
          push_socket.send(message, (last_message == &message ? 0 : ZMQ_SNDMORE) | ZMQ_NOBLOCK);
      }
    }
   protected:
    std::shared_ptr<zmq::context_t> context;
    zmq::socket_t push_socket;
    produce_function_t produce_function;
  };

  //forward message from one group of workers to the next
  class router {
    using route_function_t = std::function<void (std::list<zmq::message_t>&)>;
   public:
    router(const std::shared_ptr<zmq::context_t>& context, const char* pull_endpoint, const char* push_endpoint, const route_function_t& route_function = [](std::list<zmq::message_t>&){}):
      context(context), pull_socket(*this->context, ZMQ_PULL), push_socket(*this->context, ZMQ_PUSH), route_function(route_function) {

      int high_water_mark = 0;
      pull_socket.setsockopt(ZMQ_RCVHWM, &high_water_mark, sizeof(high_water_mark));
      pull_socket.bind(pull_endpoint);
      push_socket.setsockopt(ZMQ_SNDHWM, &high_water_mark, sizeof(high_water_mark));
      push_socket.bind(push_endpoint);
    }
    void route() {
      //keep forwarding messages
      while(true) {
        //grab all the parts of this message
        std::list<zmq::message_t> messages;
        uint64_t more;
        size_t more_size = sizeof(more);
        do {
          messages.emplace_back();
          pull_socket.recv(&messages.back());
          pull_socket.getsockopt(ZMQ_RCVMORE, &more, &more_size);
        } while(more);
        //do something to them, maybe
        route_function(messages);
        //pass them along
        const auto* last_message = &messages.back();
        for(auto& message : messages)
          push_socket.send(message, (last_message == &message ? 0 : ZMQ_SNDMORE) | ZMQ_NOBLOCK);
      }
    }
   protected:
    std::shared_ptr<zmq::context_t> context;
    zmq::socket_t pull_socket;
    zmq::socket_t push_socket;
    route_function_t route_function;
  };

  //perform an action on a message and pass to next router/collector
  class worker {
    using work_function_t = std::function<std::list<zmq::message_t> (const std::list<zmq::message_t>&)>;
   public:
    worker(const std::shared_ptr<zmq::context_t>& context, const char* pull_endpoint, const char* push_endpoint, const work_function_t& work_function):
      context(context), pull_socket(*this->context, ZMQ_PULL), push_socket(*this->context, ZMQ_PUSH), work_function(work_function) {

      int high_water_mark = 0;
      pull_socket.setsockopt(ZMQ_RCVHWM, &high_water_mark, sizeof(high_water_mark));
      pull_socket.connect(pull_endpoint);
      push_socket.setsockopt(ZMQ_SNDHWM, &high_water_mark, sizeof(high_water_mark));
      push_socket.connect(push_endpoint);
    }
    void work() {
      while(true) {
        //grab all the parts of this message
        std::list<zmq::message_t> job;
        uint64_t more;
        size_t more_size = sizeof(more);
        do {
          job.emplace_back();
          pull_socket.recv(&job.back());
          pull_socket.getsockopt(ZMQ_RCVMORE, &more, &more_size);
        } while(more);
        //do some work
        auto messages = work_function(job);
        //pass them along
        const auto* last_message = &messages.back();
        for(auto& message : messages)
          push_socket.send(message, (last_message == &message ? 0 : ZMQ_SNDMORE) | ZMQ_NOBLOCK);
      }
    }
   protected:
    std::shared_ptr<zmq::context_t> context;
    zmq::socket_t pull_socket;
    zmq::socket_t push_socket;
    work_function_t work_function;
  };

  //collects completed work after passing through the entire butterfly
  class collector {
    using collect_function_t = std::function<bool (const std::list<zmq::message_t>&)>;
   public:
    collector(const std::shared_ptr<zmq::context_t>& context, const char* pull_endpoint, const collect_function_t& collect_function):
      context(context), pull_socket(*this->context, ZMQ_PULL), collect_function(collect_function) {

      int high_water_mark = 0;
      pull_socket.setsockopt(ZMQ_RCVHWM, &high_water_mark, sizeof(high_water_mark));
      pull_socket.bind(pull_endpoint);
    }
    void collect() {
      while(true) {
        //grab all the parts of this message
        std::list<zmq::message_t> messages;
        uint64_t more;
        size_t more_size = sizeof(more);
        do {
          messages.emplace_back();
          pull_socket.recv(&messages.back());
          pull_socket.getsockopt(ZMQ_RCVMORE, &more, &more_size);
        } while(more);
        //check if we are done
        if(collect_function(messages))
          break;
      }
    }
   protected:
    std::shared_ptr<zmq::context_t> context;
    zmq::socket_t pull_socket;
    collect_function_t collect_function;
  };

}

#endif //__MESSAGING_MESSAGING_HPP__
