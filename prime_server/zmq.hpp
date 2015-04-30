#include <zmq.h>
#include <memory>
#include <stdexcept>
#include <list>
#include <cassert>


namespace zmq {

  struct context_t {
    context_t(/*TODO: add options*/) {
      //make the c context
      auto* context = zmq_ctx_new();
      if(!context)
        throw std::runtime_error(zmq_strerror(zmq_errno()));

      //wrap it in RAII goodness
      ptr.reset(context,
        [](void* context) {
          assert(zmq_ctx_destroy(context) == 0);
        });
    }
    operator void*() {
      return ptr.get();
    }
   protected:
    std::shared_ptr<void> ptr;
  };

  struct message_t {
    explicit message_t(size_t size = 0) {
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
    void reset(size_t size = 0) {
      assert(zmq_msg_close(ptr.get()) == 0);
      if(zmq_msg_init_size(ptr.get(), size) != 0)
        throw std::runtime_error(zmq_strerror(zmq_errno()));
    }
    operator zmq_msg_t*() {
      return ptr.get();
    }
    void* data() {
      return zmq_msg_data(ptr.get());
    }
    const void* data() const {
      return zmq_msg_data(const_cast<zmq_msg_t*>(ptr.get()));
    }
    size_t size() const {
      return zmq_msg_size(const_cast<zmq_msg_t*>(ptr.get()));
    }
   protected:
    std::shared_ptr<zmq_msg_t> ptr;
  };

  struct socket_t {
    socket_t(const context_t& context, int socket_type):context(context) {
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
    void setsockopt(int option, const void* value, size_t value_length) {
      if(zmq_setsockopt(ptr.get(), option, value, value_length) != 0)
        throw std::runtime_error(zmq_strerror(zmq_errno()));
    }
    //get an option from this socket
    void getsockopt(int option, void* value, size_t* value_length) {
      if(zmq_getsockopt(ptr.get(), option, value, value_length) != 0)
        throw std::runtime_error(zmq_strerror(zmq_errno()));
    }
    //connect the socket
    void connect(const char* address) {
      if(zmq_connect(ptr.get(), address) != 0)
        throw std::runtime_error(zmq_strerror(zmq_errno()));
    }
    //bind the socket
    void bind(const char* address) {
      if(zmq_bind(ptr.get(), address) != 0)
        throw std::runtime_error(zmq_strerror(zmq_errno()));
    }
    //read a single message from this socket
    bool recv(message_t& message, int flags) {
      auto byte_count = zmq_msg_recv(message, ptr.get(), flags);
      //ignore EAGAIN it just means you asked for non-blocking and there wasn't anything
      if(byte_count == -1 && zmq_errno() != EAGAIN)
        throw std::runtime_error(zmq_strerror(zmq_errno()));
      return byte_count >= 0;
    }
    //read all of the messages on this socket
    std::list<message_t> recv_all(int flags) {
      //grab all message parts
      std::list<message_t> messages;
      int more;
      size_t more_size = sizeof(more);
      do {
        messages.emplace_back();
        if(!recv(messages.back(), flags))
          messages.pop_back();
        zmq_getsockopt(ptr.get(), ZMQ_RCVMORE, &more, &more_size);
      } while(more);
      return messages;
    }
    //send some bytes
    bool send(const void* bytes, size_t count, int flags) {
      auto byte_count = zmq_send(ptr.get(), bytes, count, flags);
      //ignore EAGAIN it just means you asked for non-blocking and we couldnt send the message
      if(byte_count == -1 && zmq_errno() != EAGAIN)
        throw std::runtime_error(zmq_strerror(zmq_errno()));
      return byte_count >= 0;
    }
    //send a single message
    template <class container_t>
    bool send(const container_t& message, int flags) {
      return send(static_cast<const void*>(message.data()), message.size(), flags);
    }
    //send all the messages over this socket
    template <class container_t>
    size_t send_all(const std::list<container_t>& messages, int flags) {
      const auto* last_message = &messages.back();
      size_t total = 0;
      for(const auto& message : messages)
        total += static_cast<size_t>(send<container_t>(message, (last_message == &message ? 0 : ZMQ_SNDMORE) | flags));
      return total;
    }
    operator void*() {
      return ptr.get();
    }
   protected:
    //keep a copy of context so that, if the one used to make
    //this socket goes out of scope, we aren't screwed
    context_t context;
    std::shared_ptr<void> ptr;
  };

  //check for events on a bunch of sockets, multiplexing ftw
  using pollitem_t = zmq_pollitem_t;
  int poll(pollitem_t* items, int count, long timeout = -1) {
    int signaled_events;
    if((signaled_events = zmq_poll(items, count, timeout)) < 0)
      throw std::runtime_error(zmq_strerror(zmq_errno()));
    return signaled_events;
  }

}
