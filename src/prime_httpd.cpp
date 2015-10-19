#include "prime_server.hpp"
#include "http_protocol.hpp"
using namespace prime_server;
#include "logging.hpp"

int main(int argc, char** argv) {

  if(argc < 4) {
    logging::ERROR("Usage: " + std::string(argv[0]) +
      " [tcp|ipc]://server_listen_endpoint[:tcp_port] [tcp|ipc]://downstream_proxy_endpoint[:tcp_port] [tcp|ipc]://server_result_loopback[:tcp_port]");
    return EXIT_FAILURE;
  }

  //TODO: validate these
  std::string server_endpoint(argv[1]);
  std::string proxy_endpoint(argv[2]);
  std::string server_result_loopback(argv[3]);
  if(server_endpoint.find("://") != 3)
    logging::ERROR("bad server listen endpoint");
  if(proxy_endpoint.find("://") != 3)
    logging::ERROR("bad downstream proxy endpoint");
  if(server_result_loopback.find("://") != 3)
    logging::ERROR("bad server result loopback");

  //start it up
  zmq::context_t context;
  http_server_t server(context, server_endpoint, proxy_endpoint, server_result_loopback, true);

  //TODO: catch SIGINT for graceful shutdown
  server.serve();

  return EXIT_SUCCESS;
}
