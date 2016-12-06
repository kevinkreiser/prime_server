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
#include <condition_variable>
#include <mutex>

using namespace prime_server;

namespace {

  class testable_client_t : public netstring_client_t {
   public:
    using netstring_client_t::netstring_client_t;
    using netstring_client_t::batch_size;
  };


  //this has to catch an interrupt or we'll wait forever
  std::mutex mutex;
  std::condition_variable condition;
  class testable_worker_t : public worker_t {
   public:
    using worker_t::worker_t;
   protected:
    virtual void cancel(bool check_previous) override {
      std::unique_lock<std::mutex> lock(mutex);
      try { worker_t::cancel(check_previous); }
      catch (...) { condition.notify_one(); }
    }
  };

  void test_early() {
    zmq::context_t context;

    //server
    std::thread server(std::bind(&netstring_server_t::serve,
      netstring_server_t(context, "ipc:///tmp/test_early_server", "ipc:///tmp/test_early_proxy_upstream", "ipc:///tmp/test_early_results", "ipc:///tmp/test_early_interrupt", false)));
    server.detach();

    //we want a client that is very fickle, we need the client to send a request and then bail right away before
    //clients always work in batch mode which is to say they send n requests and then wait for n responses. to trick
    //the client into hanging up without a response we've made it so that the act of making a request sets the expected
    //number of responses to 0. this will skip the loop that tries to collect n responses because it thinks it needs 0
    {
      std::string request = netstring_entity_t::to_string("nei, will i noed");
      testable_client_t* client;
      client = new testable_client_t(context, "ipc:///tmp/test_early_server",
        [&request, &client]() {
        client->batch_size = 0;
          return std::make_pair(static_cast<const void*>(request.c_str()), request.size());
        },
        [](const void* data, size_t size) { return false; }, 1);
      client->batch();
    }

    //load balancer
    std::thread proxy(std::bind(&proxy_t::forward, proxy_t(context, "ipc:///tmp/test_early_proxy_upstream", "ipc:///tmp/test_early_proxy_downstream")));
    proxy.detach();

    //busy worker
    std::thread worker(std::bind(&testable_worker_t::work,
      testable_worker_t(context, "ipc:///tmp/test_early_proxy_downstream", "ipc:///dev/null", "ipc:///tmp/test_early_results", "ipc:///tmp/test_early_interrupt",
      [] (const std::list<zmq::message_t>& job, void*) {
        while(true);
        return worker_t::result_t{false, {netstring_entity_t::to_string("i schteck fescht")}};
      })));
    worker.detach();

    //wait to get the signal that it was intercepted
    std::unique_lock<std::mutex> lock(mutex);
    condition.wait(lock);
  }

  void test_loop() {
    zmq::context_t context;

    //server
    std::thread server(std::bind(&netstring_server_t::serve,
      netstring_server_t(context, "ipc:///tmp/test_loop_server", "ipc:///tmp/test_loop_proxy_upstream", "ipc:///tmp/test_loop_results", "ipc:///tmp/test_loop_interrupt", false)));
    server.detach();

    //we want a client that is very fickle, we need the client to send a request and then bail right away before
    //clients always work in batch mode which is to say they send n requests and then wait for n responses. to trick
    //the client into hanging up without a response we've made it so that the act of making a request sets the expected
    //number of responses to 0. this will skip the loop that tries to collect n responses because it thinks it needs 0
    std::string request = netstring_entity_t::to_string("nei, will i noed");
    testable_client_t* client;
    client = new testable_client_t(context, "ipc:///tmp/test_loop_server",
      [&request, &client]() {
      client->batch_size = 0;
        return std::make_pair(static_cast<const void*>(request.c_str()), request.size());
      },
      [](const void* data, size_t size) { return false; }, 1);
    client->batch();

    //load balancer
    std::thread proxy(std::bind(&proxy_t::forward, proxy_t(context, "ipc:///tmp/test_loop_proxy_upstream", "ipc:///tmp/test_loop_proxy_downstream")));
    proxy.detach();

    //busy worker
    std::thread worker(std::bind(&worker_t::work,
      worker_t(context, "ipc:///tmp/test_loop_proxy_downstream", "ipc:///dev/null", "ipc:///tmp/test_loop_results", "ipc:///tmp/test_loop_interrupt",
      [] (const std::list<zmq::message_t>& job, void*) {
        condition.notify_one();
        while(true) {
          try { /*cancel();*/ }
          catch (...) { condition.notify_one(); }
        }
        return worker_t::result_t{false, {netstring_entity_t::to_string("i schteck fescht")}};
      })));
    worker.detach();

    //wait for notify of working, send the cancel, wait for cancel
    std::unique_lock<std::mutex> lock(mutex);
    condition.wait(lock);
    delete client;
    condition.wait(lock);
  }

}

int main() {
  //make this whole thing bail if it doesnt finish fast
  alarm(10);

  testing::suite suite("interrupt");

  suite.test(TEST_CASE(test_early));

  suite.test(TEST_CASE(test_loop));

  return suite.tear_down();
}
