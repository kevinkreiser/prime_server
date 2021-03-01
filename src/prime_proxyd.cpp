#include "http_protocol.hpp"
#include "prime_helpers.hpp"
#include "prime_server.hpp"
using namespace prime_server;
#include "logging/logging.hpp"

int main(int argc, char** argv) {

  if (argc < 3) {
    logging::ERROR(
        "Usage: " + std::string(argv[0]) +
        " [tcp|ipc]://upstream_endpoint[:tcp_port] [tcp|ipc]://downstream_endpoint[:tcp_port]");
    return EXIT_FAILURE;
  }

  // TODO: validate these
  std::string upstream_endpoint(argv[1]);
  std::string downstream_endpoint(argv[2]);
  if (upstream_endpoint.find("://") != 3)
    logging::ERROR("bad upstream endpoint");
  if (downstream_endpoint.find("://") != 3)
    logging::ERROR("bad downstream endpoint");

  // setup the signal handler to gracefully shutdown when requested with sigterm
  unsigned int drain_seconds, shutdown_seconds;
  std::tie(drain_seconds, shutdown_seconds) = parse_quiesce_config(argc > 3 ? argv[3] : "");
  quiesce(drain_seconds, shutdown_seconds);

  // start it up
  zmq::context_t context;
  proxy_t proxy(context, upstream_endpoint, downstream_endpoint);

  // TODO: catch SIGINT for graceful shutdown
  proxy.forward();

  return EXIT_SUCCESS;
}
