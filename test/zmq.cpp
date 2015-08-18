#include "testing/testing.hpp"
#include "zmq_helpers.hpp"

#include <unistd.h>
#include <string>
#include <thread>

namespace {

  void server_thread(zmq::context_t& context, const std::string& request) {
    zmq::socket_t server(context, ZMQ_STREAM);

    int disabled = 0;
    server.setsockopt(ZMQ_SNDHWM, &disabled, sizeof(disabled));
    server.setsockopt(ZMQ_RCVHWM, &disabled, sizeof(disabled));
    #if ZMQ_VERSION_MAJOR >= 4
    #if ZMQ_VERSION_MINOR >= 1
    int enabled = 1;
    server.setsockopt(ZMQ_STREAM_NOTIFY, &enabled, sizeof(enabled));
    #endif
    #endif
    server.bind("ipc://test_server");

    server.recv_all(0);

    std::string combined_request;
    while(combined_request.size() < request.size()) {
      auto messages = server.recv_all(ZMQ_DONTWAIT);
      if(messages.size() == 0)
        continue;
      messages.pop_front();
      for(const auto& message : messages)
        combined_request.append(static_cast<const char*>(message.data()), message.size());
    }

    if(combined_request != request)
      throw std::runtime_error("Unexpected request data");
  }

  void client_thread(zmq::context_t& context, const std::string& request) {
    zmq::socket_t client(context, ZMQ_STREAM);

    int disabled = 0;
    client.setsockopt(ZMQ_SNDHWM, &disabled, sizeof(disabled));
    client.setsockopt(ZMQ_RCVHWM, &disabled, sizeof(disabled));
    #if ZMQ_VERSION_MAJOR >= 4
    #if ZMQ_VERSION_MINOR >= 1
    int enabled = 1;
    client.setsockopt(ZMQ_STREAM_NOTIFY, &enabled, sizeof(enabled));
    #endif
    #endif
    client.connect("ipc://test_server");

    client.recv_all(0);
    uint8_t identity[256];
    size_t identity_size = sizeof(identity);
    client.getsockopt(ZMQ_IDENTITY, identity, &identity_size);

    client.send(static_cast<const void*>(identity), identity_size, ZMQ_SNDMORE);
    client.send(request, 0);
  }

  void test_batch_overflow_asynchronous() {
    zmq::context_t context;

    std::string request(9000, ' ');
    for(size_t i = 0; i < request.size(); ++i)
      request[i] = (i % 95) + 32;

    std::thread server(std::bind(&server_thread, context, request));
    std::thread client(std::bind(&client_thread, context, request));

    server.join();
    client.join();
  }

  void test_batch_overflow_synchronous() {
    //instantiation
    zmq::context_t context;
    zmq::socket_t server(context, ZMQ_STREAM);
    zmq::socket_t client(context, ZMQ_STREAM);

    //connect them up
    int disabled = 0;
    server.setsockopt(ZMQ_SNDHWM, &disabled, sizeof(disabled));
    server.setsockopt(ZMQ_RCVHWM, &disabled, sizeof(disabled));
    client.setsockopt(ZMQ_SNDHWM, &disabled, sizeof(disabled));
    client.setsockopt(ZMQ_RCVHWM, &disabled, sizeof(disabled));
    #if ZMQ_VERSION_MAJOR >= 4
    #if ZMQ_VERSION_MINOR >= 1
    int enabled = 1;
    server.setsockopt(ZMQ_STREAM_NOTIFY, &enabled, sizeof(enabled));
    client.setsockopt(ZMQ_STREAM_NOTIFY, &enabled, sizeof(enabled));
    #endif
    #endif
    server.bind("ipc://test_server");
    client.connect("ipc://test_server");

    //great eachother
    server.recv_all(0);
    client.recv_all(0);
    uint8_t identity[256];
    size_t identity_size = sizeof(identity);
    client.getsockopt(ZMQ_IDENTITY, identity, &identity_size);

    //make some requests
    std::string request(8500, ' ');
    for(size_t i = 0; i < request.size(); ++i)
      request[i] = (i % 95) + 32;
    client.send(static_cast<const void*>(identity), identity_size, ZMQ_SNDMORE);
    client.send(request, 0);

    //get some requests
    std::string combined_request;
    while(combined_request.size() < request.size()) {
      auto messages = server.recv_all(ZMQ_DONTWAIT);
      if(messages.size() == 0)
        continue;
      messages.pop_front();
      for(const auto& message : messages)
        combined_request.append(static_cast<const char*>(message.data()), message.size());
    }

    if(combined_request != request)
      throw std::runtime_error("Unexpected request data");
  }

  void test_batch_overflow_router_dealer() {
    //instantiation
    zmq::context_t context;
    zmq::socket_t dealer(context, ZMQ_DEALER);
    zmq::socket_t router(context, ZMQ_ROUTER);

    //connect them up
    int disabled = 0;
    dealer.setsockopt(ZMQ_SNDHWM, &disabled, sizeof(disabled));
    dealer.setsockopt(ZMQ_RCVHWM, &disabled, sizeof(disabled));
    router.setsockopt(ZMQ_SNDHWM, &disabled, sizeof(disabled));
    router.setsockopt(ZMQ_RCVHWM, &disabled, sizeof(disabled));

    dealer.connect("ipc://test_router_dealer");
    router.bind("ipc://test_router_dealer");

    //make some requests
    size_t request_count = 100000;
    std::string r("this is just a little message but if you send a lot it adds up!");
    for(size_t i = 0; i < request_count; ++i)
      dealer.send(r, ZMQ_DONTWAIT);

    //drain the requests
    size_t got = 0;
    while(got != 2 * request_count)
      got += router.recv_all(ZMQ_DONTWAIT).size();
  }


  void test_batch_overflow() {
    //instantiation
    zmq::context_t context;
    zmq::socket_t client(context, ZMQ_STREAM);
    zmq::socket_t server(context, ZMQ_STREAM);
    zmq::socket_t dealer(context, ZMQ_DEALER);
    zmq::socket_t router(context, ZMQ_ROUTER);

    //connect them up
    int disabled = 0;
    client.setsockopt(ZMQ_SNDHWM, &disabled, sizeof(disabled));
    client.setsockopt(ZMQ_RCVHWM, &disabled, sizeof(disabled));
    server.setsockopt(ZMQ_SNDHWM, &disabled, sizeof(disabled));
    server.setsockopt(ZMQ_RCVHWM, &disabled, sizeof(disabled));
    dealer.setsockopt(ZMQ_SNDHWM, &disabled, sizeof(disabled));
    dealer.setsockopt(ZMQ_RCVHWM, &disabled, sizeof(disabled));
    router.setsockopt(ZMQ_SNDHWM, &disabled, sizeof(disabled));
    router.setsockopt(ZMQ_RCVHWM, &disabled, sizeof(disabled));
    #if ZMQ_VERSION_MAJOR >= 4
    #if ZMQ_VERSION_MINOR >= 1
    int enabled = 1;
    client.setsockopt(ZMQ_STREAM_NOTIFY, &enabled, sizeof(enabled));
    server.setsockopt(ZMQ_STREAM_NOTIFY, &enabled, sizeof(enabled));
    #endif
    #endif
    client.connect("ipc://test_server");
    server.bind("ipc://test_server");
    dealer.connect("ipc://test_router_dealer");
    router.bind("ipc://test_router_dealer");

    //great eachother
    client.recv_all(0);
    server.recv_all(0);
    uint8_t identity[256];
    size_t identity_size = sizeof(identity);
    client.getsockopt(ZMQ_IDENTITY, identity, &identity_size);

    //make some requests
    std::string request(100, ' ');
    for(size_t i = 0; i < request.size(); ++i)
      request[i] = (i % 95) + 32;
    for(size_t i = 0; i < 100; ++i) {
      client.send(static_cast<const void*>(identity), identity_size, ZMQ_SNDMORE);
      client.send(request, 0);
    }

    //get some requests
    std::string combined_request;
    size_t spot = 0;
    while(combined_request.size() < request.size()) {
      auto messages = server.recv_all(ZMQ_DONTWAIT);
      if(messages.size() == 0)
        continue;
      messages.pop_front();
      for(const auto& message : messages)
        combined_request.append(static_cast<const char*>(message.data()), message.size());
      while(combined_request.size() - spot >= 100) {
        router.send(std::string("somerequestname"), ZMQ_DONTWAIT | ZMQ_SNDMORE);
        router.send(static_cast<const void*>(&spot), sizeof(spot), ZMQ_DONTWAIT | ZMQ_SNDMORE);
        router.send(static_cast<const void*>(combined_request.c_str() + spot), 100, ZMQ_DONTWAIT);
        spot += 100;
      }
    }
  }

}

int main() {
  //make this whole thing bail if it doesnt finish fast
  alarm(30);

  testing::suite suite("zmq");

  suite.test(TEST_CASE(test_batch_overflow_synchronous));

  suite.test(TEST_CASE(test_batch_overflow_asynchronous));

  suite.test(TEST_CASE(test_batch_overflow_router_dealer));

  suite.test(TEST_CASE(test_batch_overflow));

  return suite.tear_down();
}
