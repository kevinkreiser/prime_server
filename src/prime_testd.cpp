#include <functional>
#include <iostream>
#include <list>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>

// netstrings are far easier to work with but http is a more interesting use-case
// so we just do everthing using the http protocol here
#include "prime_server/http_protocol.hpp"
#include "prime_helpers.hpp"
#include "prime_server/prime_server.hpp"
using namespace prime_server;
#include "logging/logging.hpp"

int main(int argc, char** argv) {

  if (argc < 2) {
    logging::ERROR("Usage: " + std::string(argv[0]) +
                   " server_listen_endpoint [concurrency] [drain_seconds,shutdown_seconds]");
    return 1;
  }

  // server endpoint
  std::string server_endpoint = argv[1];
  if (server_endpoint.find("://") == std::string::npos)
    logging::ERROR("Usage: " + std::string(argv[0]) +
                   " server_listen_endpoint [concurrency] [drain_seconds,shutdown_seconds]");

  // number of workers to use at each stage
  size_t worker_concurrency = 1;
  if (argc > 2)
    worker_concurrency = std::stoul(argv[2]);

  // setup the signal handler to gracefully shutdown when requested with sigterm
  unsigned int drain_seconds, shutdown_seconds;
  std::tie(drain_seconds, shutdown_seconds) = parse_quiesce_config(argc > 3 ? argv[3] : "");
  quiesce(drain_seconds, shutdown_seconds);

  // change these to tcp://known.ip.address.with:port if you want to do this across machines
  zmq::context_t context;
  std::string result_endpoint = "ipc:///tmp/result_endpoint";
  std::string request_interrupt = "ipc://request_interrupt";
  std::string proxy_endpoint = "ipc:///tmp/proxy_endpoint";

  http_server_t::shortcircuit_matcher_t shortcircuit_matcher{};
  std::string shortcircuit_response;
  shortcircuit_matcher = [&argv](const http_request_t& r) -> bool { return r.method == method_t::OPTIONS; };
  shortcircuit_response = http_response_t{200, "OK", "Testing Short Circuit"}.to_string();


  // server
  std::thread server_thread =
      std::thread(std::bind(&http_server_t::serve,
                            http_server_t(context, server_endpoint, proxy_endpoint + "_upstream",
                                          result_endpoint, request_interrupt, true,1024 * 1024 * 10,
                                          -1, {}, {}, shortcircuit_matcher, shortcircuit_response)));

  // load balancer for parsing
  std::thread echo_proxy(std::bind(&proxy_t::forward, proxy_t(context, proxy_endpoint + "_upstream",
                                                              proxy_endpoint + "_downstream")));
  echo_proxy.detach();

  // echoers
  std::list<std::thread> echo_worker_threads;
  for (size_t i = 0; i < worker_concurrency; ++i) {

    echo_worker_threads.emplace_back(
        std::bind(&worker_t::work,
                  worker_t(context, proxy_endpoint + "_downstream", "ipc:///dev/null",
                           result_endpoint, request_interrupt,
                           [](const std::list<zmq::message_t>& job, void* request_info,
                              worker_t::interrupt_function_t&) {
                             worker_t::result_t result{false, {}, {}};
                             try {

                                  // headers_t headers { 
                                  // {"Content-Type", "text/plain"},
                                  // {"Accept-Encoding", "gzip"},
                                  // {"User-Agent", "MyCustomAgent"},
                                  // {"Access-Control-Allow-Origin", "*"}
                                  // };
                               // echo
                               http_response_t response(200, 
                                                        "OK",
                                                        std::string(static_cast<const char*>(job.front().data()),
                                                        job.front().size()));
                               response.from_info(*static_cast<http_request_info_t*>(request_info));
                               result.messages = {response.to_string()};
                             } catch (const std::exception& e) {
                               // complain
                               http_response_t response(400, "Bad Request", e.what());
                               response.from_info(*static_cast<http_request_info_t*>(request_info));
                               result.messages = {response.to_string()};
                             }
                             return result;
                           })));
    echo_worker_threads.back().detach();
  }

  server_thread.join();
  return EXIT_SUCCESS;
}
