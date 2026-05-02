#include "netstring_protocol.hpp"
#include "prime_server.hpp"
#include "testing/testing.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace prime_server;

namespace {

void test_shutdown() {
  // quiesce blocks SIGTERM in the calling thread and only the dedicated sigwait thread catches it,
  // so kill(getpid(), SIGTERM) is safe -- main keeps running. with drain=1, shutting_down fires
  // after 1s. poll timeouts are capped to 1s internally so loops exit within 1s of that.
  quiesce(1);

  zmq::context_t context;
  std::atomic<bool> serve_returned{false};
  std::atomic<bool> work_returned{false};

  // request_timeout=30 is much larger than the 1s poll cap, so without the cap serve() would be
  // stuck for up to 30s before noticing shutting_down.
  std::thread server_thread([&]() {
    netstring_server_t server(context, "tcp://127.0.0.1:15708",
                              "inproc://test_shutdown_proxy_upstream",
                              "inproc://test_shutdown_results",
                              "inproc://test_shutdown_interrupt",
                              false, DEFAULT_MAX_REQUEST_SIZE, 30);
    server.serve();
    serve_returned = true;
  });
  server_thread.detach();

  // the proxy is stateless and detached -- it doesn't need to exit cleanly, so we don't
  // cap its poll or check that it returned. it just gives the worker sockets something to
  // connect to so ZMQ doesn't block on context teardown.
  std::thread proxy_thread([&]() {
    proxy_t proxy(context, "inproc://test_shutdown_proxy_upstream",
                           "inproc://test_shutdown_proxy_downstream");
    proxy.forward();
  });
  proxy_thread.detach();

  // work() poll is capped to 1s just like serve(), so it notices shutting_down promptly.
  std::thread worker_thread([&]() {
    worker_t worker(context, "inproc://test_shutdown_proxy_downstream",
                    "inproc://dev_null", "inproc://test_shutdown_results",
                    "inproc://test_shutdown_interrupt",
                    [](const std::list<zmq::message_t>&, void*,
                       worker_t::interrupt_function_t&) -> worker_t::result_t {
                      return {false, {"ok"}, {}};
                    });
    worker.work();
    work_returned = true;
  });
  worker_thread.detach();

  // let everything start up and enter their poll loops
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // quiesce handler catches this and sets shutting_down after the 1s drain
#ifdef _WIN32
  // CTRL_C_EVENT is ignored for process groups created with CREATE_NEW_PROCESS_GROUP,
  // so we must use CTRL_BREAK_EVENT to reach our own process group
  GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, GetCurrentProcessId());
#else
  kill(getpid(), SIGTERM);
#endif

  // 1s drain + up to 1s poll + 1s buffer = 3s should be plenty
  std::this_thread::sleep_for(std::chrono::seconds(3));

  if (!serve_returned)
    throw std::runtime_error("serve() did not exit within the shutdown window");
  if (!work_returned)
    throw std::runtime_error("work() did not exit within the shutdown window");
}

// Prove that a cooperative worker calling bail() in a loop is NOT notified when shutdown occurs.
// The work_function simulates a long-running request that periodically calls bail() -- the only
// cooperative cancellation point the API offers. With current code, bail() only checks per-request
// interrupts (timeout, client disconnect) and is blind to shutting_down. This test asserts the
// DESIRED behavior: bail() should throw when the process is shutting down
void test_interrupt_from_shutdown() {
  quiesce(1);

  zmq::context_t context;
  std::atomic<bool> bail_threw{false};
  std::atomic<bool> work_started{false};
  std::atomic<bool> work_returned{false};

  // server with a generous request_timeout so timeouts don't interfere
  std::thread server_thread([&]() {
    netstring_server_t server(context, "tcp://127.0.0.1:15709",
                              "inproc://test_bail_sd_proxy_up",
                              "inproc://test_bail_sd_results",
                              "inproc://test_bail_sd_interrupt", false, DEFAULT_MAX_REQUEST_SIZE,
                              30);
    server.serve();
  });
  server_thread.detach();

  // proxy gives worker sockets something to connect to
  std::thread proxy_thread([&]() {
    proxy_t proxy(context, "inproc://test_bail_sd_proxy_up",
                  "inproc://test_bail_sd_proxy_down");
    proxy.forward();
  });
  proxy_thread.detach();

  // worker whose work_function cooperatively calls bail() in a loop
  std::thread worker_thread([&]() {
    worker_t worker(
        context, "inproc://test_bail_sd_proxy_down", "inproc://dev_null",
        "inproc://test_bail_sd_results", "inproc://test_bail_sd_interrupt",
        [&](const std::list<zmq::message_t>&, void*,
            worker_t::interrupt_function_t& bail) -> worker_t::result_t {
          work_started = true;
          // a well-behaved work function that uses bail() as its ONLY cancellation check.
          // it does NOT check shutting_down() directly -- the whole point is to prove that
          // bail() alone is sufficient to learn about shutdown. the 10s deadline is just a
          // safety net so the test doesn't hang if the fix is missing.
          auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
          while (std::chrono::steady_clock::now() < deadline) {
            try {
              bail();
            } catch (...) {
              bail_threw = true;
              break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
          }
          return {false, {"done"}, {}};
        });
    worker.work();
    work_returned = true;
  });
  worker_thread.detach();

  // let everything start and enter their poll loops
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // fire-and-forget client to trigger the work_function
  std::thread client_thread([&]() {
    std::string request = netstring_entity_t::to_string("test");
    netstring_client_t client(
        context, "tcp://127.0.0.1:15709",
        [&request]() {
          return std::make_pair(static_cast<const void*>(request.c_str()), request.size());
        },
        [](const void*, size_t) { return false; }, 1);
    try {
      client.batch();
    } catch (...) {}
  });
  client_thread.detach();

  // wait for the worker to actually be inside work_function
  while (!work_started)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // trigger shutdown: draining for 1s, then shutting_down fires
#ifdef _WIN32
  GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, GetCurrentProcessId());
#else
  kill(getpid(), SIGTERM);
#endif

  // 1s drain + up to 1s poll + 2s buffer
  std::this_thread::sleep_for(std::chrono::seconds(4));

  if (!work_returned)
    throw std::runtime_error("work() did not exit within the shutdown window");

  // bail() should have thrown to notify the worker about shutdown, but with current code it
  // only checks per-request interrupts and never learns about shutting_down.
  if (!bail_threw)
    throw std::runtime_error("bail() did not interrupt the worker on shutdown -- "
                             "the interrupt function is blind to shutting_down");
}

} // namespace

int main() {
  // make this whole thing bail if it doesnt finish fast
  testing::set_timeout(60);

  testing::suite suite("shutdown");

  suite.test(SUBPROCESS_TEST_CASE(test_shutdown));

  suite.test(SUBPROCESS_TEST_CASE(test_interrupt_from_shutdown));

  return suite.tear_down();
}
