#include "http_protocol.hpp"
#include "prime_server.hpp"
using namespace prime_server;
#include "logging.hpp"

#include <cstring>
#include <string>

int main(int argc, char** argv) {

  if (argc < 5) {
    logging::ERROR(
        "Usage: " + std::string(argv[0]) +
        " [tcp|ipc]://server_listen_endpoint[:tcp_port] [tcp|ipc]://downstream_proxy_endpoint[:tcp_port] [tcp|ipc]://server_result_loopback[:tcp_port] [tcp|ipc]://server_request_interrupt[:tcp_port] [enable_logging] [max_request_size_bytes] [request_timeout_seconds] [/health_check_endpoint]");
    return EXIT_FAILURE;
  }

  // TODO: validate these
  std::string server_endpoint(argv[1]);
  std::string proxy_endpoint(argv[2]);
  std::string server_result_loopback(argv[3]);
  std::string server_request_interrupt(argv[4]);
  if (server_endpoint.find("://") != 3)
    logging::ERROR("bad server listen endpoint");
  if (proxy_endpoint.find("://") != 3)
    logging::ERROR("bad downstream proxy endpoint");
  if (server_result_loopback.find("://") != 3)
    logging::ERROR("bad server result loopback");
  if (server_request_interrupt.find("://") != 3)
    logging::ERROR("bad server request interrupt");

  // default to logging requests/responses
  bool log = argc < 6 || strcasecmp(argv[5], "false") != 0;

  // default to 10mb
  size_t max_request_size_bytes = 1024 * 1024 * 10;
  try {
    if (argc > 6)
      max_request_size_bytes = std::stoul(argv[6]);
  } catch (...) {}

  // default to no timeout
  uint32_t request_timeout_seconds = -1;
  try {
    if (argc > 7)
      request_timeout_seconds = std::stoul(argv[7]);
  } catch (...) {}

  // default to no health check, if you want it, it looks like:
  http_server_t::health_check_matcher_t health_check_matcher{};
  std::string health_check_response;
  if (argc > 8) {
    health_check_matcher = [&argv](const http_request_t& r) -> bool { return r.path == argv[8]; };
    // TODO: make this configurable
    health_check_response = http_response_t{200, "OK"}.to_string();
  }

  // start it up
  zmq::context_t context;
  http_server_t server(context, server_endpoint, proxy_endpoint, server_result_loopback,
                       server_request_interrupt, log, max_request_size_bytes, request_timeout_seconds,
                       health_check_matcher, health_check_response);

  // TODO: catch SIGINT for graceful shutdown
  server.serve();

  return EXIT_SUCCESS;
}
