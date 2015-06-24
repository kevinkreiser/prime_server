#include "testing/testing.hpp"
#include "zmq_helpers.hpp"

#include <unistd.h>
#include <string>

namespace {
  void test_batch_overflow() {
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

    //get some responses
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

}

int main() {
  //make this whole thing bail if it doesnt finish fast
  alarm(1);

  testing::suite suite("zmq");

  suite.test(TEST_CASE(test_batch_overflow));

  return suite.tear_down();
}
