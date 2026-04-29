#include <cstdlib>

#include "prime_server.hpp"
using namespace prime_server;
#include "logging/logging.hpp"

int main(int argc, char** argv) {

  if (argc < 3) {
    logging::ERROR(
        "Usage: " + std::string(argv[0]) +
        " [tcp|ipc]://upstream_endpoint[:tcp_port] [tcp|ipc]://downstream_endpoint[:tcp_port] [drain_seconds]");
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
  quiesce(argc > 3 ? std::stoul(argv[3]) : 28);

  // start it up
  zmq::context_t context;
  proxy_t proxy(context, upstream_endpoint, downstream_endpoint);

  proxy.forward();
  return EXIT_SUCCESS;
}
