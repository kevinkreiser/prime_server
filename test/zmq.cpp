#include "testing/testing.hpp"
#include "zmq_helpers.hpp"

#include <string>
#include <thread>
#include <unistd.h>

namespace {

std::string readable_string(size_t size = 200) {
  std::string s(size, ' ');
  for (size_t i = 0; i < s.size(); ++i)
    s[i] = (i % 95) + 32;
  return s;
}

void server_thread(zmq::context_t& context, const std::string& request, size_t iterations) {
  zmq::socket_t server(context, ZMQ_STREAM);

  int disabled = 0;
  server.setsockopt(ZMQ_SNDHWM, &disabled, sizeof(disabled));
  server.setsockopt(ZMQ_RCVHWM, &disabled, sizeof(disabled));
  server.bind("ipc:///tmp/test_server");

  zmq::message_t identity;

  std::string compounded;
  for (size_t i = 0; i < iterations; ++i)
    compounded += request;

  std::string combined_request;
  while (combined_request.size() < compounded.size()) {
    auto messages = server.recv_all(ZMQ_DONTWAIT);
    if (messages.size() == 0)
      continue;
    if (identity.size() != 0 && messages.front() != identity)
      throw std::runtime_error("Identity frame mismatch");
    identity = std::move(messages.front());
    messages.pop_front();
    for (const auto& message : messages)
      combined_request.append(static_cast<const char*>(message.data()), message.size());
  }

  if (combined_request != compounded)
    throw std::runtime_error("Unexpected request data");
}

void client_thread(zmq::context_t& context, const std::string& request, size_t iterations) {
  zmq::socket_t client(context, ZMQ_STREAM);

  int disabled = 0;
  client.setsockopt(ZMQ_SNDHWM, &disabled, sizeof(disabled));
  client.setsockopt(ZMQ_RCVHWM, &disabled, sizeof(disabled));
  client.connect("ipc:///tmp/test_server");
  client.recv_all(0);

  uint8_t identity[256];
  size_t identity_size = sizeof(identity);
  client.getsockopt(ZMQ_IDENTITY, identity, &identity_size);

  for (size_t i = 0; i < iterations; ++i) {
    client.send(static_cast<const void*>(identity), identity_size, ZMQ_SNDMORE);
    client.send(request, 0);
  }
}

void test_batch_overflow_asynchronous() {
  zmq::context_t context;

  std::string request = readable_string(200);

  std::thread server(std::bind(&server_thread, context, request, 1000000));
  std::thread client(std::bind(&client_thread, context, request, 1000000));

  server.join();
  client.join();
}

void test_batch_overflow_synchronous() {
  // instantiation
  zmq::context_t context;
  zmq::socket_t server(context, ZMQ_STREAM);
  zmq::socket_t client(context, ZMQ_STREAM);

  // connect them up
  int disabled = 0;
  server.setsockopt(ZMQ_SNDHWM, &disabled, sizeof(disabled));
  server.setsockopt(ZMQ_RCVHWM, &disabled, sizeof(disabled));
  client.setsockopt(ZMQ_SNDHWM, &disabled, sizeof(disabled));
  client.setsockopt(ZMQ_RCVHWM, &disabled, sizeof(disabled));
  server.bind("ipc:///tmp/test_server");
  client.connect("ipc:///tmp/test_server");

  // great eachother
  auto client_identity = std::move(server.recv_all(0).front());
  client.recv_all(0);
  uint8_t identity[256];
  size_t identity_size = sizeof(identity);
  client.getsockopt(ZMQ_IDENTITY, identity, &identity_size);

  // make some requests
  std::string request = readable_string(10000);
  std::string compounded;
  for (size_t i = 0; i < 100; ++i) {
    client.send(identity, identity_size, ZMQ_SNDMORE | ZMQ_DONTWAIT);
    client.send(request, ZMQ_DONTWAIT);
    compounded += request;
  }

  // get some requests
  std::string combined_request;
  while (combined_request.size() < compounded.size()) {
    auto messages = server.recv_all(ZMQ_DONTWAIT);
    if (messages.size() == 0)
      continue;
    if (messages.front() != client_identity)
      throw std::runtime_error("Identity frame mismatch");
    messages.pop_front();
    for (const auto& message : messages)
      combined_request.append(static_cast<const char*>(message.data()), message.size());
  }

  if (combined_request != compounded)
    throw std::runtime_error("Unexpected request data");
}

void test_batch_overflow_router_dealer() {
  // instantiation
  zmq::context_t context;
  zmq::socket_t dealer(context, ZMQ_DEALER);
  zmq::socket_t router(context, ZMQ_ROUTER);

  // connect them up
  int disabled = 0;
  dealer.setsockopt(ZMQ_SNDHWM, &disabled, sizeof(disabled));
  dealer.setsockopt(ZMQ_RCVHWM, &disabled, sizeof(disabled));
  router.setsockopt(ZMQ_SNDHWM, &disabled, sizeof(disabled));
  router.setsockopt(ZMQ_RCVHWM, &disabled, sizeof(disabled));

  zmq::message_t identity(static_cast<void*>(new unsigned char[7]{'a', 'b', 'c', 'd', 'e', 'f', 'g'}),
                          7);
  dealer.setsockopt(ZMQ_IDENTITY, identity.data(), identity.size());

  dealer.connect("ipc:///tmp/test_router_dealer");
  router.bind("ipc:///tmp/test_router_dealer");

  // make lots of little requests
  size_t request_count = 100000;
  std::string r("this is just a little message but if you send a lot, it adds up!");
  for (size_t i = 0; i < request_count; ++i)
    dealer.send(r, ZMQ_DONTWAIT);

  // drain the requests
  size_t got = 0;
  while (got != 2 * request_count) {
    auto m = router.recv_all(ZMQ_DONTWAIT);
    got += m.size();
    if (m.size() == 0)
      continue;
    if (m.size() != 2)
      throw std::runtime_error("Should only be an identity frame and a message");
    if (m.front() != identity)
      throw std::runtime_error("Identity frame has the wrong data");
    if (std::string(static_cast<const char*>(m.back().data()), m.back().size()) != r)
      throw std::runtime_error("Message frame has the wrong data");
  }

  // make a fair amount of large requests
  request_count = 10000;
  r = readable_string(10000);
  for (size_t i = 0; i < request_count; ++i)
    dealer.send(r, ZMQ_DONTWAIT);

  // drain the requests
  got = 0;
  while (got != 2 * request_count) {
    auto m = router.recv_all(ZMQ_DONTWAIT);
    got += m.size();
    if (m.size() == 0)
      continue;
    if (m.size() != 2)
      throw std::runtime_error("Should only be an identity frame and a message");
    if (m.front() != identity)
      throw std::runtime_error("Identity frame has the wrong data");
    if (std::string(static_cast<const char*>(m.back().data()), m.back().size()) != r)
      throw std::runtime_error("Message frame has the wrong data");
  }
}

void test_batch_overflow() {
  // instantiation
  zmq::context_t context;
  zmq::socket_t client(context, ZMQ_STREAM);
  zmq::socket_t server(context, ZMQ_STREAM);
  zmq::socket_t dealer(context, ZMQ_DEALER);
  zmq::socket_t router(context, ZMQ_ROUTER);

  // connect them up
  int disabled = 0;
  client.setsockopt(ZMQ_SNDHWM, &disabled, sizeof(disabled));
  client.setsockopt(ZMQ_RCVHWM, &disabled, sizeof(disabled));
  server.setsockopt(ZMQ_SNDHWM, &disabled, sizeof(disabled));
  server.setsockopt(ZMQ_RCVHWM, &disabled, sizeof(disabled));
  dealer.setsockopt(ZMQ_SNDHWM, &disabled, sizeof(disabled));
  dealer.setsockopt(ZMQ_RCVHWM, &disabled, sizeof(disabled));
  router.setsockopt(ZMQ_SNDHWM, &disabled, sizeof(disabled));
  router.setsockopt(ZMQ_RCVHWM, &disabled, sizeof(disabled));
  client.connect("ipc:///tmp/test_server");
  server.bind("ipc:///tmp/test_server");
  dealer.connect("ipc:///tmp/test_router_dealer");
  router.bind("ipc:///tmp/test_router_dealer");

  // great eachother
  server.recv_all(0);
  client.recv_all(0);
  uint8_t identity[256];
  size_t identity_size = sizeof(identity);
  client.getsockopt(ZMQ_IDENTITY, identity, &identity_size);

  // make some requests
  std::string request = readable_string(100);
  for (size_t i = 0; i < 100; ++i) {
    client.send(static_cast<const void*>(identity), identity_size, ZMQ_SNDMORE);
    client.send(request, 0);
  }

  // get some requests
  std::string combined_request;
  size_t spot = 0;
  while (combined_request.size() < request.size()) {
    auto messages = server.recv_all(ZMQ_DONTWAIT);
    if (messages.size() == 0)
      continue;
    messages.pop_front();
    for (const auto& message : messages)
      combined_request.append(static_cast<const char*>(message.data()), message.size());
    while (combined_request.size() - spot >= 100) {
      router.send(std::string("somerequestname"), ZMQ_DONTWAIT | ZMQ_SNDMORE);
      router.send(static_cast<const void*>(&spot), sizeof(spot), ZMQ_DONTWAIT | ZMQ_SNDMORE);
      router.send(static_cast<const void*>(combined_request.c_str() + spot), 100, ZMQ_DONTWAIT);
      spot += 100;
    }
  }
}

void test_stream_notify() {
  // server
  zmq::context_t context;
  zmq::socket_t server(context, ZMQ_STREAM);
  server.bind("ipc:///tmp/test_server");

  // client
  auto* client = new zmq::socket_t(context, ZMQ_STREAM);
  client->connect("ipc:///tmp/test_server");
  uint8_t identity[256];
  size_t identity_size = sizeof(identity);
  client->getsockopt(ZMQ_IDENTITY, identity, &identity_size);

  // check the connection
  auto conn = server.recv_all(0);
  if (conn.size() != 2 || conn.front().size() != identity_size || conn.back().size() != 0)
    throw std::logic_error(
        "Connection should have garnered an identity frame followed by a blank message");
  // the server and the client use different identities to refer to the client
  zmq::message_t client_identity = std::move(conn.front());

  // check identity
  client->send(identity, identity_size, ZMQ_SNDMORE | ZMQ_DONTWAIT);
  client->send(identity, identity_size, ZMQ_DONTWAIT);
  auto msg = server.recv_all(0);
  if (msg.size() != 2 || msg.front().size() != client_identity.size() || msg.back().size() == 0)
    throw std::logic_error(
        "Connection should have garnered an identity frame followed by a non blank message");
  if (msg.front() != client_identity)
    throw std::logic_error("The client identity frame didn't match");

  // check the disconnection
  delete client;
  auto disconn = server.recv_all(0);
  if (disconn.size() != 2 || disconn.front().size() != identity_size || disconn.back().size() != 0)
    throw std::logic_error("Disconnection should have garnered a blank message");
  if (disconn.front() != client_identity)
    throw std::logic_error("The client identity frame didn't match");
}

void test_pub_sub() {
  zmq::context_t context;

  // publish forever
  std::thread publisher(
      [](zmq::context_t& context) {
        zmq::socket_t pub(context, ZMQ_PUB);
        int disabled = 0;
        pub.setsockopt(ZMQ_SNDHWM, &disabled, sizeof(disabled));
        pub.bind("ipc:///tmp/test_pub_sub");
        while (true)
          pub.send("listen to this", strlen("listen to this"), ZMQ_DONTWAIT);
      },
      std::ref(context));
  publisher.detach();

  // wait to hear something
  std::thread subscriber(
      [](zmq::context_t& context) {
        zmq::socket_t sub(context, ZMQ_SUB);
        int disabled = 0;
        sub.setsockopt(ZMQ_RCVHWM, &disabled, sizeof(disabled));
        sub.setsockopt(ZMQ_SUBSCRIBE, "", 0);
        sub.connect("ipc:///tmp/test_pub_sub");

        zmq::pollitem_t item{sub, 0, ZMQ_POLLIN, 0};
        zmq::poll(&item, 1, -1);

        if (!(item.revents & ZMQ_POLLIN))
          throw std::logic_error("Poller triggered but not for the right socket");
        auto messages = sub.recv_all(0);
        if (messages.size() != 1)
          throw std::logic_error("Wrong number of published messages");
        if (messages.front().str() != "listen to this")
          throw std::logic_error("Wrong message contents");
      },
      std::ref(context));
  subscriber.join();
}

} // namespace

int main() {
  // make this whole thing bail if it doesnt finish fast
  alarm(60);

  testing::suite suite("zmq");

  suite.test(TEST_CASE(test_batch_overflow_synchronous));

  suite.test(TEST_CASE(test_batch_overflow_asynchronous));

  suite.test(TEST_CASE(test_batch_overflow_router_dealer));

  suite.test(TEST_CASE(test_batch_overflow));

  suite.test(TEST_CASE(test_stream_notify));

  suite.test(TEST_CASE(test_pub_sub));

  return suite.tear_down();
}
