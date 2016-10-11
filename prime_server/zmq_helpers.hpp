#ifndef __ZMQ_HELPERS_HPP__
#define __ZMQ_HELPERS_HPP__

#include <zmq.h>
#include <memory>
#include <stdexcept>
#include <list>
#include <cassert>
#include <cstring>

namespace zmq {

  struct context_t {
    context_t(/*TODO: add options*/);
    operator void*();
   protected:
    std::shared_ptr<void> ptr;
  };

  struct message_t {
    explicit message_t(void* data, size_t size, void (*free_function)(void*,void*) = [](void* ptr, void* hint){delete[] static_cast<unsigned char*>(ptr);});
    explicit message_t(size_t size = 0);
    void reset(size_t size = 0);
    operator zmq_msg_t*();
    void* data();
    const void* data() const;
    size_t size() const;
    bool operator==(const message_t& other) const;
    bool operator!=(const message_t& other) const;
   protected:
    std::shared_ptr<zmq_msg_t> ptr;
  };

  struct socket_t {
    socket_t(const context_t& context, int socket_type);
    //set an option on this socket
    void setsockopt(int option, const void* value, size_t value_length);
    //get an option from this socket
    void getsockopt(int option, void* value, size_t* value_length);
    //connect the socket
    void connect(const char* address);
    //bind the socket
    void bind(const char* address);
    //read a single message from this socket
    bool recv(message_t& message, int flags);
    //read all of the messages on this socket
    std::list<message_t> recv_all(int flags);
    //send some bytes
    bool send(const void* bytes, size_t count, int flags);
    //send a single message
    template <class container_t>
    bool send(const container_t& message, int flags);
    //send all the messages over this socket
    template <class container_t>
    size_t send_all(const std::list<container_t>& messages, int flags);
    operator void*();
   protected:
    //keep a copy of context so that, if the one used to make
    //this socket goes out of scope, we aren't screwed
    context_t context;
    std::shared_ptr<void> ptr;
  };

  //check for events on a bunch of sockets, multiplexing ftw
  using pollitem_t = zmq_pollitem_t;
  int poll(pollitem_t* items, int count, long timeout = -1);

}

namespace std {
  //make messages hashable in the same way that strings are
  template<>
  struct hash<zmq::message_t> : public __hash_base<size_t, zmq::message_t> {
    size_t operator()(const zmq::message_t& __m) const noexcept
    {
      return std::_Hash_impl::hash(__m.data(), __m.size());
    }
  };
}

#endif //__ZMQ_HELPERS_HPP__
