#include "zmq_helpers.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <ctime>
#include <czmq.h>
#include <random>

namespace {

constexpr char uuid_chars[] = {'a', 'b', 'c', 'd', 'e', 'f', '0', '1',
                               '2', '3', '4', '5', '6', '7', '8', '9'};

}

namespace zmq {

context_t::context_t(/*TODO: add options*/) {
  // make the c context
  auto* context = zmq_ctx_new();
  if (!context)
    throw std::runtime_error(zmq_strerror(zmq_errno()));

  // wrap it in RAII goodness
  ptr.reset(context, [](void* context) {
    auto ret = zmq_ctx_term(context);
    if (ret != 0)
      throw std::runtime_error("zmq_ctx_term error: " + std::to_string(errno));
  });
}
context_t::operator void*() {
  return ptr.get();
}

message_t::message_t(void* data, size_t size, void (*free_function)(void*, void*)) {
  // make the c message
  zmq_msg_t* message = new zmq_msg_t();
  if (zmq_msg_init_data(message, data, size, free_function, nullptr) != 0)
    throw std::runtime_error(zmq_strerror(zmq_errno()));

  // wrap it in RAII goodness
  ptr.reset(message, [](zmq_msg_t* message) {
    auto ret = zmq_msg_close(message);
    if (ret != 0)
      throw std::runtime_error("zmq_msg_close error: " + std::to_string(errno));
    delete message;
  });
}
message_t::message_t(size_t size, const void* data) {
  // make the c message
  zmq_msg_t* message = new zmq_msg_t();
  if (zmq_msg_init_size(message, size) != 0)
    throw std::runtime_error(zmq_strerror(zmq_errno()));
  // copy the data into it
  if (size && data)
    memcpy(zmq_msg_data(message), data, size);

  // wrap it in RAII goodness
  ptr.reset(message, [](zmq_msg_t* message) {
    auto ret = zmq_msg_close(message);
    if (ret != 0)
      throw std::runtime_error("zmq_msg_close error: " + std::to_string(errno));
    delete message;
  });
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

std::string message_t::str() const {
  return std::string(static_cast<const char*>(zmq_msg_data(const_cast<zmq_msg_t*>(ptr.get()))),
                     zmq_msg_size(const_cast<zmq_msg_t*>(ptr.get())));
}

bool message_t::operator==(const message_t& other) const {
  return size() == other.size() && std::memcmp(data(), other.data(), size()) == 0;
}

bool message_t::operator!=(const message_t& other) const {
  return size() != other.size() || std::memcmp(data(), other.data(), size()) != 0;
}

socket_t::socket_t(const context_t& context, int socket_type) : context(context) {
  // make the c socket
  auto* socket = zmq_socket(this->context, socket_type);
  if (!socket)
    throw std::runtime_error(zmq_strerror(zmq_errno()));

  // wrap it in RAII goodness
  ptr.reset(socket, [](void* socket) {
    auto ret = zmq_close(socket);
    if (ret != 0)
      throw std::runtime_error("zmq_close error: " + std::to_string(errno));
  });
}
// set an option on this socket
void socket_t::setsockopt(int option, const void* value, size_t value_length) {
  if (zmq_setsockopt(ptr.get(), option, value, value_length) != 0)
    throw std::runtime_error(zmq_strerror(zmq_errno()));
}
// get an option from this socket
void socket_t::getsockopt(int option, void* value, size_t* value_length) {
  if (zmq_getsockopt(ptr.get(), option, value, value_length) != 0)
    throw std::runtime_error(zmq_strerror(zmq_errno()));
}
// connect the socket
void socket_t::connect(const char* address) {
  if (zmq_connect(ptr.get(), address) != 0)
    throw std::runtime_error(zmq_strerror(zmq_errno()));
}
// bind the socket
void socket_t::bind(const char* address) {
  if (zmq_bind(ptr.get(), address) != 0)
    throw std::runtime_error(zmq_strerror(zmq_errno()));
}
// read a single message from this socket
bool socket_t::recv(message_t& message, int flags) {
  auto byte_count = zmq_msg_recv(message, ptr.get(), flags);
  // ignore EAGAIN it just means you asked for non-blocking and there wasn't anything
  if (byte_count == -1 && zmq_errno() != EAGAIN)
    throw std::runtime_error(zmq_strerror(zmq_errno()));
  return byte_count >= 0;
}
// read all of the messages on this socket
std::list<message_t> socket_t::recv_all(int flags) {
  // grab all message parts
  std::list<message_t> messages;
  int more;
  size_t more_size = sizeof(more);
  do {
    messages.emplace_back();
    if (!recv(messages.back(), flags))
      messages.pop_back();
    getsockopt(ZMQ_RCVMORE, &more, &more_size);
  } while (more);
  return messages;
}
// send some bytes
bool socket_t::send(const void* bytes, size_t count, int flags) {
  auto byte_count = zmq_send(ptr.get(), bytes, count, flags);
  // ignore EAGAIN it just means you asked for non-blocking and we couldnt send the message
  if (byte_count == -1 && zmq_errno() != EAGAIN)
    throw std::runtime_error(zmq_strerror(zmq_errno()));
  return byte_count >= 0;
}
// send a single message
template <class container_t>
bool socket_t::send(const container_t& message, int flags) {
  return send(static_cast<const void*>(message.data()), message.size(), flags);
}
// send all the messages over this socket
template <class container_t>
size_t socket_t::send_all(const std::list<container_t>& messages, int flags) {
  const auto* last_message = &messages.back();
  size_t total = 0;
  for (const auto& message : messages)
    total += static_cast<size_t>(
        send<container_t>(message, (last_message == &message ? 0 : ZMQ_SNDMORE) | flags));
  return total;
}
// for polling
socket_t::operator void*() {
  return ptr.get();
}

struct beacon_t::cheshire_cat_t {
  cheshire_cat_t(uint16_t port)
      : distribution(static_cast<size_t>(0), sizeof(uuid_chars) - 1), actor(nullptr),
        socket(nullptr) {
    // make the actor with the beacon function
    actor.reset(zactor_new(zbeacon, nullptr), [](zactor_t* a) { zactor_destroy(&a); });
    if (!actor)
      throw std::runtime_error("Beacon not supported");
    // set up the socket
    zsock_send(actor.get(), "si", "CONFIGURE", static_cast<int>(port));
    // get the ip
    char* name = zstr_recv(actor.get());
    if (name == nullptr)
      throw std::runtime_error("Beacon not supported");
    ip.assign(name);
    free(name);
    if (!ip.size())
      throw std::runtime_error("Beacon not supported");
    // keep the socket for polling
    socket = zsock_resolve(zactor_sock(actor.get()));
    generator.seed(std::time(nullptr));
  }
  ~cheshire_cat_t() {
  }
  std::string rand_uuid(size_t size) {
    std::string uuid;
    for (size_t i = 0; i < size; ++i) {
      auto c = uuid_chars[distribution(generator)];
      uuid.push_back(c);
    }
    return uuid;
  }
  services_t purge_clique(std::time_t max_age = 60) {
    services_t dropped;
    auto now = std::time(nullptr);
    auto end = punch_card.begin();
    for (; end != punch_card.end(); ++end) {
      // youre too young to die!
      if (now - end->first < max_age)
        break;
      // hit the pike codger!
      dropped.emplace(*end->second);
      clique.erase(end->second->first);
      services.erase(end->second);
    }
    // clean up the ones we axed
    punch_card.erase(punch_card.begin(), end);
    return dropped;
  }
  bool join_clique(const std::string& endpoint, const std::string& uuid) {
    bool joined = true;
    auto now = std::time(nullptr);
    auto member = clique.find(endpoint);
    // erase their last check in
    if (member != clique.cend()) {
      joined = false;
      services.erase(member->second->second);
      punch_card.erase(member->second);
      clique.erase(member);
    }
    // check them in
    auto inserted = services.insert({endpoint, uuid});
    punch_card.emplace_back(std::make_pair(now, inserted.first));
    clique.emplace(endpoint, std::prev(punch_card.end()));
    return joined;
  }

  // for uuid generation
  std::default_random_engine generator;
  std::uniform_int_distribution<size_t> distribution;

  // info about myself
  std::shared_ptr<zactor_t> actor;
  void* socket;
  std::string ip;

  // info about others
  using punch_card_t = std::list<std::pair<std::time_t, services_t::iterator>>;
  using clique_t = std::unordered_map<std::string, punch_card_t::iterator>;
  services_t services;
  punch_card_t punch_card;
  clique_t clique;
};

beacon_t::beacon_t(uint16_t discovery_port) : pimpl(new cheshire_cat_t(discovery_port)) {
}
// ip address
const std::string& beacon_t::get_ip() const {
  return pimpl->ip;
}
// start broadcasting
void beacon_t::broadcast(uint16_t service_port, int interval) {
  service_port = htons(service_port);
  // make a zre protocol message
  char zre[22];
  memcpy(zre, "ZRE\1", 4);
  memcpy(zre + 4, pimpl->rand_uuid(16).c_str(), 16);
  memcpy(zre + 20, &service_port, sizeof(service_port));
  zsock_send(pimpl->actor.get(), "sbi", "PUBLISH", zre, sizeof(zre), interval);
}
// stop broadcasting
void beacon_t::silence() {
  zstr_sendx(pimpl->actor.get(), "SILENCE", nullptr);
}
// start listening for signals
void beacon_t::subscribe(const std::string& filter) {
  zsock_send(pimpl->actor.get(), "sb", "SUBSCRIBE", filter.c_str(), filter.size());
}
// stop listening for signals
void beacon_t::unsubscribe() {
  zstr_sendx(pimpl->actor.get(), "UNSUBSCRIBE", nullptr);
}
// update the services
std::pair<services_t, services_t> beacon_t::update(bool activity) {
  // these just came or just went
  services_t joined, dropped;
  // if we're expecting someone we'll let them check in
  char* ip = activity ? zstr_recv(pimpl->actor.get()) : nullptr;
  if (ip) {
    zframe_t* frame = zframe_recv(pimpl->actor.get());
    // right size and header
    if (zframe_size(frame) == 22 && !memcmp(zframe_data(frame), "ZRE\1", 4)) {
      auto* opaque = static_cast<void*>(zframe_data(frame));
      std::string uuid(static_cast<char*>(opaque) + 4, 16);
      uint16_t port = ntohs(*(static_cast<uint16_t*>(opaque) + 10));
      std::string endpoint("tcp://");
      endpoint += ip;
      endpoint += ":" + std::to_string(port);
      if (pimpl->join_clique(endpoint, uuid))
        joined.emplace(endpoint, uuid);
    }
    zframe_destroy(&frame);
    zstr_free(&ip);
  }
  // check out people who arent checking in much lately
  dropped = pimpl->purge_clique();
  return std::make_pair(std::move(joined), std::move(dropped));
}
const services_t& beacon_t::services() const {
  return pimpl->services;
}
// for polling
beacon_t::operator void*() {
  return pimpl->socket;
}

// check for events on a bunch of sockets, multiplexing ftw
int poll(pollitem_t* items, int count, long timeout) {
  int signaled_events;
  if ((signaled_events = zmq_poll(items, count, timeout)) < 0)
    throw std::runtime_error(zmq_strerror(zmq_errno()));
  return signaled_events;
}

// make a random port in suggested range
uint16_t random_port() {
  std::default_random_engine generator(std::time(nullptr));
  std::uniform_int_distribution<uint16_t> distribution(49152, 65535);
  return distribution(generator);
}

// explicit instantiations for templated sending of data
template bool socket_t::send<std::string>(const std::string&, int);
template bool socket_t::send<zmq::message_t>(const zmq::message_t&, int);
template size_t socket_t::send_all<std::string>(const std::list<std::string>&, int);
template size_t socket_t::send_all<zmq::message_t>(const std::list<zmq::message_t>&, int);

} // namespace zmq

namespace std {
size_t hash<zmq::message_t>::operator()(const zmq::message_t& m) const noexcept {
  auto sv = std::string_view(static_cast<const char*>(m.data()), m.size());
  return std::hash<std::string_view>{}(sv);
}
} // namespace std
