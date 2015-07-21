#include "testing/testing.hpp"
#include "prime_server.hpp"
#include "netstring_protocol.hpp"

#include <unistd.h>
#include <functional>
#include <memory>
#include <unordered_set>
#include <thread>
#include <iterator>
#include <cstdlib>
#include <cstring>

using namespace prime_server;

namespace {

  void netstring_client_work(zmq::context_t& context) {
    //client makes requests and gets back responses in a batch fashion
    const size_t total = 100;
    size_t requests = 0;
    size_t received = 0;
    std::string request;
    netstring_client_t client(context, "ipc://test_netstring_server",
      [&requests, &request]() {
        //we want more requests
        if(requests < total) {
          ++requests;
          request = std::string(80,' ');
          netstring_request_t::format(request);
        }//blank request means we are done
        else
          request.clear();
        return std::make_pair(static_cast<const void*>(request.c_str()), request.size());
      },
      [&received](const void* data, size_t size) {
        return ++received < total;
      }, 100
    );
    //request and receive
    client.batch();
  }

  void test_parallel_clients() {

    zmq::context_t context;

    //server
    std::thread server(std::bind(&netstring_server_t::serve,
      netstring_server_t(context, "ipc://test_netstring_server", "ipc://test_netstring_proxy_upstream", "ipc://test_netstring_results")));
    server.detach();

    //load balancer for parsing
    std::thread proxy(std::bind(&proxy_t::forward,
      proxy_t(context, "ipc://test_netstring_proxy_upstream", "ipc://test_netstring_proxy_downstream")));
    proxy.detach();

    //make a bunch of clients
    std::thread client1(std::bind(&netstring_client_work, context));
    client1.join();
  }

}

int main() {
  //make this whole thing bail if it doesnt finish fast
  alarm(60);

  testing::suite suite("netstring");

  suite.test(TEST_CASE(test_parallel_clients));

  return suite.tear_down();
}
