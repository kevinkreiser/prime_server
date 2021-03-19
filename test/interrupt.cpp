#include "netstring_protocol.hpp"
#include "prime_server.hpp"
#include "testing/testing.hpp"

#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <unordered_set>

using namespace prime_server;

namespace {
// use these to manipulate flow control
std::mutex mutex;
std::condition_variable condition;

class testable_client_t : public netstring_client_t {
public:
  using netstring_client_t::batch_size;
  using netstring_client_t::netstring_client_t;
};

// this has to catch an interrupt or we'll wait forever
class testable_worker_t : public worker_t {
public:
  using worker_t::worker_t;

protected:
  virtual void handle_interrupt(bool force_check) override {
    std::unique_lock<std::mutex> lock(mutex);
    condition.notify_one();
    condition.wait(lock);
    // I dont like doing this but... with pub sub delivery isnt gauranteed so its best to wait around
    std::this_thread::sleep_for(std::chrono::seconds(1));
    try {
      worker_t::handle_interrupt(force_check);
    } catch (...) { condition.notify_one(); }
  }
};

void test_early() {
  zmq::context_t context;

  // server
  std::thread server(std::bind(&netstring_server_t::serve,
                               netstring_server_t(context, "ipc:///tmp/test_early_server",
                                                  "ipc:///tmp/test_early_proxy_upstream",
                                                  "ipc:///tmp/test_early_results",
                                                  "ipc:///tmp/test_early_interrupt", false)));
  server.detach();

  // load balancer
  std::thread proxy(
      std::bind(&proxy_t::forward, proxy_t(context, "ipc:///tmp/test_early_proxy_upstream",
                                           "ipc:///tmp/test_early_proxy_downstream")));
  proxy.detach();

  // busy worker
  std::thread worker(
      std::bind(&testable_worker_t::work,
                testable_worker_t(context, "ipc:///tmp/test_early_proxy_downstream",
                                  "ipc:///dev/null", "ipc:///tmp/test_early_results",
                                  "ipc:///tmp/test_early_interrupt",
                                  [](const std::list<zmq::message_t>&, void*,
                                     const worker_t::interrupt_function_t&) -> worker_t::result_t {
                                    while (true) {}
                                  })));
  worker.detach();

  // we want a client that is very fickle, we need the client to send a request and then bail right
  // away before clients always work in batch mode which is to say they send n requests and then wait
  // for n responses. to trick the client into hanging up without a response we've made it so that the
  // act of making a request sets the expected number of responses to 0. this will skip the loop that
  // tries to collect n responses because it thinks it needs 0
  std::string request = netstring_entity_t::to_string("nei, will i noed");
  testable_client_t* client = new testable_client_t(
      context, "ipc:///tmp/test_early_server",
      [&request, &client]() {
        client->batch_size = 0;
        return std::make_pair(static_cast<const void*>(request.c_str()), request.size());
      },
      [](const void*, size_t) { return false; }, 1);
  client->batch();

  // wait for job, disconnect, wait for cancel
  std::unique_lock<std::mutex> lock(mutex);
  condition.wait(lock);
  delete client;
  condition.notify_one();
  condition.wait(lock);
}

void test_loop() {
  zmq::context_t context;

  // server
  std::thread server(std::bind(&netstring_server_t::serve,
                               netstring_server_t(context, "ipc:///tmp/test_loop_server",
                                                  "ipc:///tmp/test_loop_proxy_upstream",
                                                  "ipc:///tmp/test_loop_results",
                                                  "ipc:///tmp/test_loop_interrupt", false)));
  server.detach();

  // we want a client that is very fickle, we need the client to send a request and then bail right
  // away clients always work in batch mode which is to say they send n requests and then wait for n
  // responses. to trick the client into hanging up without a response we've made it so that the act
  // of making a request sets the expected number of responses to 0. this will skip the loop that
  // tries to collect n responses because it thinks it needs 0
  std::string request = netstring_entity_t::to_string("nei, will i noed");
  testable_client_t* client;
  client = new testable_client_t(
      context, "ipc:///tmp/test_loop_server",
      [&request, &client]() {
        client->batch_size = 0;
        return std::make_pair(static_cast<const void*>(request.c_str()), request.size());
      },
      [](const void*, size_t) { return false; }, 1);
  client->batch();

  // load balancer
  std::thread proxy(
      std::bind(&proxy_t::forward, proxy_t(context, "ipc:///tmp/test_loop_proxy_upstream",
                                           "ipc:///tmp/test_loop_proxy_downstream")));
  proxy.detach();

  // busy worker
  std::thread worker(
      std::bind(&worker_t::work,
                worker_t(context, "ipc:///tmp/test_loop_proxy_downstream", "ipc:///dev/null",
                         "ipc:///tmp/test_loop_results", "ipc:///tmp/test_loop_interrupt",
                         [](const std::list<zmq::message_t>&, void*,
                            const worker_t::interrupt_function_t& interrupt) -> worker_t::result_t {
                           condition.notify_one();
                           while (true) {
                             try {
                               interrupt();
                             } catch (...) { condition.notify_one(); }
                           }
                         })));
  worker.detach();

  // wait for notify of working, send the cancel, wait for cancel
  std::unique_lock<std::mutex> lock(mutex);
  condition.wait(lock);
  delete client;
  condition.wait(lock);
}

void test_timeout() {
  zmq::context_t context;

  // server
  std::thread server(std::bind(&netstring_server_t::serve,
                               netstring_server_t(context, "ipc:///tmp/test_timeout_server",
                                                  "ipc:///tmp/test_timeout_proxy_upstream",
                                                  "ipc:///tmp/test_timeout_results",
                                                  "ipc:///tmp/test_timeout_interrupt", false,
                                                  DEFAULT_MAX_REQUEST_SIZE, 1)));
  server.detach();

  // load balancer
  std::thread proxy(
      std::bind(&proxy_t::forward, proxy_t(context, "ipc:///tmp/test_timeout_proxy_upstream",
                                           "ipc:///tmp/test_timeout_proxy_downstream")));
  proxy.detach();

  // busy worker
  std::thread worker(
      std::bind(&worker_t::work,
                worker_t(context, "ipc:///tmp/test_timeout_proxy_downstream", "ipc:///dev/null",
                         "ipc:///tmp/test_timeout_results", "ipc:///tmp/test_timeout_interrupt",
                         [](const std::list<zmq::message_t>&, void*,
                            const worker_t::interrupt_function_t&) -> worker_t::result_t {
                           while (true) {}
                         })));
  worker.detach();

  std::string request = netstring_entity_t::to_string("wart uf mi");
  testable_client_t client(
      context, "ipc:///tmp/test_timeout_server",
      [&request]() {
        return std::make_pair(static_cast<const void*>(request.c_str()), request.size());
      },
      [](const void* data, size_t size) {
        auto response = netstring_entity_t::from_string(static_cast<const char*>(data), size);
        if (response.body.substr(0, 7) != "TIMEOUT")
          throw std::runtime_error("Expected TIMEOUT response!");
        return false;
      },
      1);
  client.batch();
}

} // namespace

int main() {
  // make this whole thing bail if it doesnt finish fast
  alarm(60);

  testing::suite suite("interrupt");

  suite.test(TEST_CASE(test_early));

  suite.test(TEST_CASE(test_loop));

  suite.test(TEST_CASE(test_timeout));

  return suite.tear_down();
}
