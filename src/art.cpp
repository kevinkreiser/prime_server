//prime_server guts
#include <prime_server/prime_server.hpp>
#include <prime_server/http_protocol.hpp>
using namespace prime_server;

//nuts and bolts required
#include <thread>
#include <functional>
#include <chrono>
#include <string>
#include <list>
#include <vector>
#include <csignal>

// server_endpoint uses tcp:// because ZMQ_STREAM requires a network transport (tcp or ipc).
// internal endpoints use inproc:// since server, proxy, and workers share one process.
// on Windows, ipc:// (Unix domain sockets) is unavailable so tcp:// is the only option for
// server-facing sockets; inproc:// works everywhere for in-process communication.
const std::string server_endpoint = "tcp://*:8002";
const std::string result_endpoint = "inproc://result_endpoint";
const std::string request_interrupt = "inproc://request_interrupt";
const std::string proxy_endpoint = "inproc://proxy_endpoint";

//assortment of artisional content
const std::vector<std::string> art = { "(_,_)", "(_|_)", "(_*_)",
                                       "(‿ˠ‿)", "(‿ꜟ‿)", "(‿ε‿)" };

//actually serve up content
worker_t::result_t art_work(const std::list<zmq::message_t>& job, void* request_info,
                            worker_t::interrupt_function_t&) {
  //false means this is going back to the client, there is no next stage of the pipeline
  worker_t::result_t result{false, {}, {}};
  //this type differs per protocol hence the void* fun
  auto& info = *static_cast<http_request_info_t*>(request_info);
  http_response_t response;
  try {
    //TODO: actually use/validate the request parameters
    auto request = http_request_t::from_string(
      static_cast<const char*>(job.front().data()), job.front().size());
    //get your art here
    response = http_response_t(200, "OK", art[info.id % art.size()]);
  }
  catch(const std::exception& e) {
    //complain
    response = http_response_t(400, "Bad Request", e.what());
  }
  //does some tricky stuff with headers and different versions of http
  response.from_info(info);
  //formats the response to protocal that the client will understand
  result.messages.emplace_back(response.to_string());
  return result;
}

int main(void) {
  //orchestrators, such as systemd, k8s, etc., will ask your application to shutdown gracefully via SIGTERM
  //you must exit before their configured deadline (TimeoutStopSec, terminationGracePeriodSeconds) or get SIGKILL'd
  //assuming a 30 second deadline, we give 28s to the workers to finish off any outstanding requests
  //we stop the server/worker threads and main uses the remaining 2 seconds to clean up and exit normally
  quiesce(28);

  zmq::context_t context;

  //http server, false turns off request/response logging
  std::thread server = std::thread(std::bind(&http_server_t::serve,
    http_server_t(context, server_endpoint, proxy_endpoint + "_upstream", result_endpoint,
                  request_interrupt, false)));

  //load balancer
  std::thread proxy(
    std::bind(&proxy_t::forward,
      proxy_t(context, proxy_endpoint + "_upstream", proxy_endpoint + "_downstream")));

  //workers
  auto worker_concurrency = std::max<size_t>(1, std::thread::hardware_concurrency());
  std::list<std::thread> workers;
  for(size_t i = 0; i < worker_concurrency; ++i) {
    //worker function could be defined inline here via lambda, it could be std::bind'd to an instance method
    //or simply just a free function like we have here
    workers.emplace_back(std::bind(&worker_t::work,
      worker_t(context, proxy_endpoint + "_downstream", "inproc://no_endpoint", result_endpoint,
               request_interrupt, &art_work)));
  }

  //join all threads before main returns so nothing outlives stack-local resources
  server.join();
  for(auto& t : workers)
    t.join();
  proxy.join();
  return 0;
}
