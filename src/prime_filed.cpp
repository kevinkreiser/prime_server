#include <cstdlib>
#include <exception>
#include <functional>
#include <list>
#include <string>
#include <thread>

#include "http_protocol.hpp"
#include "http_util.hpp"
#include "logging/logging.hpp"
#include "prime_server.hpp"
#include "zmq_helpers.hpp"

using namespace prime_server;

std::string root = "./";

worker_t::result_t
disk_work(const std::list<zmq::message_t>& job, void* request_info, worker_t::interrupt_function_t&) {
  worker_t::result_t result{false, {}, {}};
  try {
    // check the disk
    auto request =
        http_request_t::from_string(static_cast<const char*>(job.front().data()), job.front().size());
    return http::disk_result(request, *static_cast<http_request_info_t*>(request_info), root);
  } catch (const std::exception& e) {
    http_response_t response(400, "Bad Request", e.what());
    response.from_info(*static_cast<http_request_info_t*>(request_info));
    result.messages = {response.to_string()};
  }
  return result;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    logging::ERROR(
        "Usage: " + std::string(argv[0]) +
        " server_listen_endpoint [root_dir] [drain_seconds] [/health_check_endpoint]");
    return 1;
  }

  // server endpoint
  std::string server_endpoint = argv[1];
  if (server_endpoint.find("://") == std::string::npos) {
    logging::ERROR(
        "Usage: " + std::string(argv[0]) +
        " server_listen_endpoint [root_dir] [drain_seconds] [/health_check_endpoint]");
    return 1;
  }

  // root dir
  if (argc > 2)
    root = argv[2];

  // setup the signal handler to gracefully shutdown when requested with sigterm
  quiesce(argc > 3 ? std::stoul(argv[3]) : 28);

  // default to no health check, if one is provided its just the path and the canned response is OK
  http_server_t::health_check_matcher_t health_check_matcher{};
  std::string health_check_response;
  if (argc > 4) {
    health_check_matcher = [&argv](const http_request_t& r) -> bool { return r.path == argv[4]; };
    // TODO: make this configurable
    health_check_response = http_response_t{200, "OK"}.to_string();
  }

  // inproc:// works within one process; use tcp:// to split components across machines or processes
  // on linux ipc:// is a faster alternative to tcp for multiprocess mode, windows doesn't support it
  zmq::context_t context;
  std::string result_endpoint = "inproc://result_endpoint";
  std::string request_interrupt = "inproc://request_interrupt";
  std::string proxy_endpoint = "inproc://proxy_endpoint";

  // server
  std::thread server = std::thread(
      std::bind(&http_server_t::serve,
                http_server_t(context, server_endpoint, proxy_endpoint + "_upstream", result_endpoint,
                              request_interrupt, true, DEFAULT_MAX_REQUEST_SIZE,
                              DEFAULT_REQUEST_TIMEOUT, health_check_matcher, health_check_response)));

  // load balancer for file serving
  std::thread file_proxy(std::bind(&proxy_t::forward, proxy_t(context, proxy_endpoint + "_upstream",
                                                              proxy_endpoint + "_downstream")));
  // file serving thread
  std::thread file_worker(
      std::bind(&worker_t::work, worker_t(context, proxy_endpoint + "_downstream", "inproc://dev_null",
                                          result_endpoint, request_interrupt,
                                          std::bind(&disk_work, std::placeholders::_1,
                                                    std::placeholders::_2, std::placeholders::_3))));

  // wait for all the threads to get a shutdown signal and exit, then main can clean up whatever its allocated
  server.join();
  file_worker.join();
  file_proxy.join();
  return EXIT_SUCCESS;
}
