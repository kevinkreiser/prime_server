#include <fstream>
#include <streambuf>
#include <string>

#include "http_protocol.hpp"
#include "prime_helpers.hpp"
#include "prime_server.hpp"
using namespace prime_server;
#include "logging/logging.hpp"

int main(int argc, char** argv) {

  if (argc < 5) {
    logging::ERROR(
        "Usage: " + std::string(argv[0]) +
        " [tcp|ipc]://upstream_proxy_endpoint[:tcp_port] [tcp|ipc]://downstream_proxy_endpoint[:tcp_port] [tcp|ipc]://server_result_loopback[:tcp_port] [tcp|ipc]://server_request_interrupt[:tcp_port] [drain_seconds,shutdown_seconds]");
    return EXIT_FAILURE;
  }

  // TODO: validate these
  std::string upstream_proxy_endpoint(argv[1]);
  std::string downstream_proxy_endpoint(argv[2]);
  std::string server_result_loopback(argv[3]);
  std::string server_request_interrupt(argv[4]);
  if (upstream_proxy_endpoint.find("://") != 3)
    logging::ERROR("bad upstream proxy endpoint");
  if (downstream_proxy_endpoint.find("://") != 3)
    logging::ERROR("bad downstream proxy endpoint");
  if (server_result_loopback.find("://") != 3)
    logging::ERROR("bad server result loopback");
  if (server_request_interrupt.find("://") != 3)
    logging::ERROR("bad server request interrupt");

  // setup the signal handler to gracefully shutdown when requested with sigterm
  unsigned int drain_seconds, shutdown_seconds;
  std::tie(drain_seconds, shutdown_seconds) = parse_quiesce_config(argc > 5 ? argv[5] : "");
  quiesce(drain_seconds, shutdown_seconds);

  // start it up
  zmq::context_t context;
  worker_t worker(context, upstream_proxy_endpoint, downstream_proxy_endpoint, server_result_loopback,
                  server_request_interrupt,
                  [](const std::list<zmq::message_t>& messages, void* request_info,
                     const worker_t::interrupt_function_t&) {
                    auto request =
                        http_request_t::from_string(static_cast<const char*>(messages.front().data()),
                                                    messages.front().size());

                    worker_t::result_t result{false, {}, {}};
                    try {
                      // TODO: bail if its too large or..
                      // make a list of jobs, one for each chunk of a chunked encoding response
                      // throw them back into the proxy at the top and have the worker pop one off
                      // each time. this would require a worker being able to both respond and forward
                      // though..
                      // TODO: use const unordered map of extension to mime-type and set a decent
                      // header
                      size_t pos = 0;
                      while (pos < request.path.size() &&
                             (request.path[pos] == '/' || request.path[pos] == '.'))
                        ++pos;

                      std::ifstream stream(request.path.c_str() + pos, std::ios_base::in);
                      stream.seekg(0, std::ios::end);
                      std::string body;
                      body.reserve(stream.tellg());
                      stream.seekg(0, std::ios::beg);
                      body.assign((std::istreambuf_iterator<char>(stream)),
                                  std::istreambuf_iterator<char>());

                      http_response_t response(200, "OK", body);
                      response.from_info(*static_cast<http_request_info_t*>(request_info));
                      result.messages.emplace_back(response.to_string());
                    } catch (...) {
                      http_response_t response(404, "Not Found");
                      response.from_info(*static_cast<http_request_info_t*>(request_info));
                      result.messages.emplace_back(response.to_string());
                    }
                    return result;
                  });

  worker.work();
  return EXIT_SUCCESS;
}
