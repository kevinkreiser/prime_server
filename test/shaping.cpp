#include "netstring_protocol.hpp"
#include "prime_server.hpp"
#include "testing/testing.hpp"

#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <memory>
#include <thread>
#include <unistd.h>
#include <unordered_set>

using namespace prime_server;

namespace {

class testable_proxy_t : public prime_server::proxy_t {
public:
  using proxy_t::proxy_t;

protected:
  // since we only ever have 2 workers below (A and B) and we want the test to be deterministic we
  // only listen for requests when both are available. this allows the proxy to always pick the right
  // one and not be forced to just go with whichever is available
  int expire() override {
    return fifo.size() == 2 ? 2 : 1;
  }
};

void netstring_client_work(zmq::context_t& context,
                           const std::string& request,
                           std::list<std::string>& responses,
                           const std::string& endpoint,
                           const size_t total,
                           const size_t batch_size) {
  // client makes requests and gets back responses in a batch fashion
  auto request_str = netstring_entity_t::to_string(request);
  netstring_client_t client(
      context, endpoint,
      [&responses, request_str, total]() -> std::pair<const void*, size_t> {
        // we want more requests
        if (responses.size() < total)
          return std::make_pair(static_cast<const void*>(request_str.c_str()), request_str.size());
        // blank request means we are done
        return std::make_pair(nullptr, 0);
      },
      [&responses, total](const void* data, size_t size) {
        // get the result and tell if there is more or not
        auto response = netstring_entity_t::from_string(static_cast<const char*>(data), size);
        responses.push_back(response.body);
        return responses.size() < total;
      },
      batch_size);
  // request and receive
  client.batch();
}

void test_unshaped() {
  zmq::context_t context;

  // server
  std::thread server(std::bind(&netstring_server_t::serve,
                               netstring_server_t(context, "ipc:///tmp/test_unshaped_server",
                                                  "ipc:///tmp/test_unshaped_proxy_upstream",
                                                  "ipc:///tmp/test_unshaped_results",
                                                  "ipc:///tmp/test_unshaped_interrupt", false)));
  server.detach();

  // load balancer for parsing
  std::thread proxy(std::bind(&proxy_t::forward,
                              testable_proxy_t(context, "ipc:///tmp/test_unshaped_proxy_upstream",
                                               "ipc:///tmp/test_unshaped_proxy_downstream")));
  proxy.detach();

  // a or b workers
  for (const auto& response : {std::string("A"), std::string("B")}) {
    std::thread worker(
        std::bind(&worker_t::work,
                  worker_t(
                      context, "ipc:///tmp/test_unshaped_proxy_downstream", "ipc:///dev/null",
                      "ipc:///tmp/test_unshaped_results", "ipc:///tmp/test_unshaped_interrupt",
                      [response](const std::list<zmq::message_t>&, void*,
                                 const worker_t::interrupt_function_t&) {
                        worker_t::result_t result{false,
                                                  {netstring_entity_t::to_string(response)},
                                                  response};
                        return result;
                      },
                      []() {}, response)));
    worker.detach();
  }

  // make a client that makes one request at a time, so we can have our requests bounce between A and
  // B workers
  std::list<std::string> responses;
  std::thread client(std::bind(&netstring_client_work, std::ref(context), "A", std::ref(responses),
                               "ipc:///tmp/test_unshaped_server", 10000, 1));
  client.join();
  size_t as = 0;
  size_t bs = 0;
  for (const auto& response : responses) {
    as += response == "A";
    bs += response == "B";
  }
  if (as != bs)
    throw std::logic_error("Traffic should have been evenly distributed but A had " +
                           std::to_string(as) + " and B had " + std::to_string(bs));
}

void test_shaped() {
  zmq::context_t context;

  // server
  std::thread server(std::bind(&netstring_server_t::serve,
                               netstring_server_t(context, "ipc:///tmp/test_shaped_server",
                                                  "ipc:///tmp/test_shaped_proxy_upstream",
                                                  "ipc:///tmp/test_shaped_results",
                                                  "ipc:///tmp/test_shaped_interrupt", false)));
  server.detach();

  // load balancer for parsing that favors heartbeats (ie workers) based on whats in the job to be
  // forwarded returning a nullptr means you dont have a preference
  std::thread proxy(
      std::bind(&proxy_t::forward,
                testable_proxy_t(context, "ipc:///tmp/test_shaped_proxy_upstream",
                                 "ipc:///tmp/test_shaped_proxy_downstream",
                                 [](const std::list<zmq::message_t>& heart_beats,
                                    const std::list<zmq::message_t>& job) -> const zmq::message_t* {
                                   // have a look at each heartbeat
                                   for (const auto& heart_beat : heart_beats) {
                                     // so the heartbeat is a single char either A or B
                                     const auto& beat_type =
                                         static_cast<const char*>(heart_beat.data())[0];
                                     // but the job is in netstring format, so either 1:A, or 1:B,
                                     const auto& job_type =
                                         static_cast<const char*>(job.front().data())[2];
                                     // do we like this heartbeat
                                     if (beat_type == job_type)
                                       return &heart_beat;
                                   }
                                   // all of the heartbeats sucked so pick whichever
                                   return nullptr;
                                 })));
  proxy.detach();

  // A or B workers
  for (const auto& response : {std::string("A"), std::string("B")}) {
    std::thread worker(
        std::bind(&worker_t::work,
                  worker_t(
                      context, "ipc:///tmp/test_shaped_proxy_downstream", "ipc:///dev/null",
                      "ipc:///tmp/test_shaped_results", "ipc:///tmp/test_shaped_interrupt",
                      [response](const std::list<zmq::message_t>&, void*,
                                 const worker_t::interrupt_function_t&) {
                        worker_t::result_t result{false,
                                                  {netstring_entity_t::to_string(response)},
                                                  response};
                        return result;
                      },
                      []() {}, response)));
    worker.detach();
  }

  // make a client that makes one request at a time, so we can always be sure to have an A worker
  // ready
  std::list<std::string> responses;
  std::thread client(std::bind(&netstring_client_work, std::ref(context), "A", std::ref(responses),
                               "ipc:///tmp/test_shaped_server", 10000, 1));
  client.join();
  size_t as = 0;
  size_t bs = 0;
  for (const auto& response : responses) {
    as += response == "A";
    bs += response == "B";
  }
  if (as != 10000 || bs != 0)
    throw std::logic_error("Only the A worker should have answered requests but A had " +
                           std::to_string(as) + " and B had " + std::to_string(bs));
}

} // namespace

int main() {
  // make this whole thing bail if it doesnt finish fast
  alarm(240);

  testing::suite suite("shaping");

  suite.test(TEST_CASE(test_unshaped));

  suite.test(TEST_CASE(test_shaped));

  return suite.tear_down();
}
