#ifndef __ZMQ_HELPERS_HPP__
#define __ZMQ_HELPERS_HPP__

#include <zmq.h>
#include <memory>
#include <stdexcept>
#include <list>
#include <cassert>
#include <cstring>
#include <string>
#include <unordered_map>

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
    std::string str() const;
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
    //for polling
    operator void*();
   protected:
    //keep a copy of context so that, if the one used to make
    //this socket goes out of scope, we aren't screwed
    context_t context;
    std::shared_ptr<void> ptr;
  };

  //all of this stuff is implemented in czmq which means
  //the interface is completely different (actor pattern).
  //the beacon is made of a zmq 'command' socket which you
  //send messages to, to control the beacon. it also has
  //a udp socket to communicate to other beacons. a thread
  //runs its own zmq poll loop to handle both sockets
  using services_t = std::unordered_map<std::string, std::string>;
  struct beacon_t {
    beacon_t(uint16_t discovery_port = 5670);
    //ip address
    const std::string& get_ip() const;
    //start broadcasting
    void broadcast(uint16_t service_port, int interval = 2000);
    //stop broadcasting
    void silence();
    //start listening for signals
    void subscribe(const std::string& filter = "");
    //stop listening for signals
    void unsubscribe();
    //update the services and return which just joined and which just dropped
    std::pair<services_t, services_t> update(bool activity);
    //return current list of services
    const services_t& services() const;
    //for polling
    operator void*();
  protected:
    struct cheshire_cat_t;
    std::shared_ptr<cheshire_cat_t> pimpl;
  };

  //check for events on a bunch of sockets, multiplexing ftw
  using pollitem_t = zmq_pollitem_t;
  int poll(pollitem_t* items, int count, long timeout = -1);

  //get a random port in IANA suggested range
  uint16_t random_port();
}

#endif //__ZMQ_HELPERS_HPP__
