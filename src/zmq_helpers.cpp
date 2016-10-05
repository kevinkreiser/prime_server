#include "zmq_helpers.hpp"
#include <string>

namespace zmq {

    context_t::context_t(/*TODO: add options*/) {
      //make the c context
      auto* context = zmq_ctx_new();
      if(!context)
        throw std::runtime_error(zmq_strerror(zmq_errno()));

      //wrap it in RAII goodness
      ptr.reset(context,
        [](void* context) {
          assert(zmq_ctx_term(context) == 0);
        });
    }
    context_t::operator void*() {
      return ptr.get();
    }

    message_t::message_t(void* data, size_t size, void (*free_function)(void*,void*)) {
      //make the c message
      zmq_msg_t* message = new zmq_msg_t();
      if(zmq_msg_init_data(message, data, size, free_function, nullptr) != 0)
        throw std::runtime_error(zmq_strerror(zmq_errno()));

      //wrap it in RAII goodness
      ptr.reset(message,
        [](zmq_msg_t* message) {
          assert(zmq_msg_close(message) == 0);
          delete message;
        });
    }
    message_t::message_t(size_t size) {
      //make the c message
      zmq_msg_t* message = new zmq_msg_t();
      if(zmq_msg_init_size(message, size) != 0)
        throw std::runtime_error(zmq_strerror(zmq_errno()));

      //wrap it in RAII goodness
      ptr.reset(message,
        [](zmq_msg_t* message) {
          assert(zmq_msg_close(message) == 0);
          delete message;
        });
    }
    void message_t::reset(size_t size) {
      assert(zmq_msg_close(ptr.get()) == 0);
      if(zmq_msg_init_size(ptr.get(), size) != 0)
        throw std::runtime_error(zmq_strerror(zmq_errno()));
    }
    message_t::operator zmq_msg_t*() {
      return ptr.get();
    }
    void* message_t::data() {
      return zmq_msg_data(ptr.get());
    }
    const void* message_t::data() const {
      return zmq_msg_data(const_cast<zmq_msg_t*>(ptr.get()));
    }
    size_t message_t::size() const {
      return zmq_msg_size(const_cast<zmq_msg_t*>(ptr.get()));
    }

    bool message_t::operator==(const message_t& other) const {
      return std::memcmp(data(), other.data(), size()) == 0;
    }

    bool message_t::operator!=(const message_t& other) const {
      return std::memcmp(data(), other.data(), size()) != 0;
    }

    socket_t::socket_t(const context_t& context, int socket_type):context(context) {
      //make the c socket
      auto* socket = zmq_socket(this->context, socket_type);
      if(!socket)
        throw std::runtime_error(zmq_strerror(zmq_errno()));

      //wrap it in RAII goodness
      ptr.reset(socket,
        [](void* socket){
          assert(zmq_close(socket) == 0);
        });
    }
    //set an option on this socket
    void socket_t::setsockopt(int option, const void* value, size_t value_length) {
      if(zmq_setsockopt(ptr.get(), option, value, value_length) != 0)
        throw std::runtime_error(zmq_strerror(zmq_errno()));
    }
    //get an option from this socket
    void socket_t::getsockopt(int option, void* value, size_t* value_length) {
      if(zmq_getsockopt(ptr.get(), option, value, value_length) != 0)
        throw std::runtime_error(zmq_strerror(zmq_errno()));
    }
    //connect the socket
    void socket_t::connect(const char* address) {
      if(zmq_connect(ptr.get(), address) != 0)
        throw std::runtime_error(zmq_strerror(zmq_errno()));
    }
    //bind the socket
    void socket_t::bind(const char* address) {
      if(zmq_bind(ptr.get(), address) != 0)
        throw std::runtime_error(zmq_strerror(zmq_errno()));
    }
    //read a single message from this socket
    bool socket_t::recv(message_t& message, int flags) {
      auto byte_count = zmq_msg_recv(message, ptr.get(), flags);
      //ignore EAGAIN it just means you asked for non-blocking and there wasn't anything
      if(byte_count == -1 && zmq_errno() != EAGAIN)
        throw std::runtime_error(zmq_strerror(zmq_errno()));
      return byte_count >= 0;
    }
    //read all of the messages on this socket
    std::list<message_t> socket_t::recv_all(int flags) {
      //grab all message parts
      std::list<message_t> messages;
      int more;
      size_t more_size = sizeof(more);
      do {
        messages.emplace_back();
        if(!recv(messages.back(), flags))
          messages.pop_back();
        getsockopt(ZMQ_RCVMORE, &more, &more_size);
      } while(more);
      return messages;
    }
    //send some bytes
    bool socket_t::send(const void* bytes, size_t count, int flags) {
      auto byte_count = zmq_send(ptr.get(), bytes, count, flags);
      //ignore EAGAIN it just means you asked for non-blocking and we couldnt send the message
      if(byte_count == -1 && zmq_errno() != EAGAIN)
        throw std::runtime_error(zmq_strerror(zmq_errno()));
      return byte_count >= 0;
    }
    //send a single message
    template <class container_t>
    bool socket_t::send(const container_t& message, int flags) {
      return send(static_cast<const void*>(message.data()), message.size(), flags);
    }
    //send all the messages over this socket
    template <class container_t>
    size_t socket_t::send_all(const std::list<container_t>& messages, int flags) {
      const auto* last_message = &messages.back();
      size_t total = 0;
      for(const auto& message : messages)
        total += static_cast<size_t>(send<container_t>(message, (last_message == &message ? 0 : ZMQ_SNDMORE) | flags));
      return total;
    }
    socket_t::operator void*() {
      return ptr.get();
    }

  //check for events on a bunch of sockets, multiplexing ftw
  int poll(pollitem_t* items, int count, long timeout) {
    int signaled_events;
    if((signaled_events = zmq_poll(items, count, timeout)) < 0)
      throw std::runtime_error(zmq_strerror(zmq_errno()));
    return signaled_events;
  }

  //explicit instantiations for templated sending of data
  template bool socket_t::send<std::string>(const std::string&,int);
  template bool socket_t::send<zmq::message_t>(const zmq::message_t&,int);
  template size_t socket_t::send_all<std::string>(const std::list<std::string>&,int);
  template size_t socket_t::send_all<zmq::message_t>(const std::list<zmq::message_t>&,int);

}
