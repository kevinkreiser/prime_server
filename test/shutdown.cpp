#include "netstring_protocol.hpp"
#include "prime_server.hpp"
#include "testing/testing.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>
#include <unistd.h>

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
    netstring_server_t server(context, "ipc:///tmp/test_shutdown_server",
                              "ipc:///tmp/test_shutdown_proxy_upstream",
                              "ipc:///tmp/test_shutdown_results",
                              "ipc:///tmp/test_shutdown_interrupt",
                              false, DEFAULT_MAX_REQUEST_SIZE, 30);
    server.serve();
    serve_returned = true;
  });
  server_thread.detach();

  // the proxy is stateless and detached -- it doesn't need to exit cleanly, so we don't
  // cap its poll or check that it returned. it just gives the worker sockets something to
  // connect to so ZMQ doesn't block on context teardown.
  std::thread proxy_thread([&]() {
    proxy_t proxy(context, "ipc:///tmp/test_shutdown_proxy_upstream",
                           "ipc:///tmp/test_shutdown_proxy_downstream");
    proxy.forward();
  });
  proxy_thread.detach();

  // work() poll is capped to 1s just like serve(), so it notices shutting_down promptly.
  std::thread worker_thread([&]() {
    worker_t worker(context, "ipc:///tmp/test_shutdown_proxy_downstream",
                    "ipc:///dev/null", "ipc:///tmp/test_shutdown_results",
                    "ipc:///tmp/test_shutdown_interrupt",
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
  kill(getpid(), SIGTERM);

  // 1s drain + up to 1s poll + 1s buffer = 3s should be plenty
  std::this_thread::sleep_for(std::chrono::seconds(3));

  if (!serve_returned)
    throw std::runtime_error("serve() did not exit within the shutdown window");
  if (!work_returned)
    throw std::runtime_error("work() did not exit within the shutdown window");
}

} // namespace

int main() {
  // make this whole thing bail if it doesnt finish fast
  alarm(60);

  testing::suite suite("shutdown");

  suite.test(TEST_CASE(test_shutdown));

  return suite.tear_down();
}
