#include <prime_server/prime_server.hpp>
#include <prime_server/http_protocol.hpp>
#include <prime_server/http_util.hpp>
#include <prime_server/zmq_helpers.hpp>
#include <prime_server/logging.hpp>

#include <string>
#include <list>
#include <exception>
#include <thread>
#include <chrono>
#include <functional>
#include <csignal>

using namespace prime_server;

std::string root = "./";

worker_t::result_t disk_work(const std::list<zmq::message_t>& job, void* request_info, worker_t::interrupt_function_t&) {
  worker_t::result_t result{false};
  try {
    //check the disk
    auto request = http_request_t::from_string(static_cast<const char*>(job.front().data()), job.front().size());
    return http::disk_result(request, *static_cast<http_request_info_t*>(request_info), root);
  }
  catch(const std::exception& e) {
    http_response_t response(400, "Bad Request", e.what());
    response.from_info(*static_cast<http_request_info_t*>(request_info));
    result.messages = {response.to_string()};
  }
  return result;
}

int main(int argc, char** argv) {
  if(argc < 2) {
    logging::ERROR("Usage: " + std::string(argv[0]) + " server_listen_endpoint [root_dir]");
    return 1;
  }

  //server endpoint
  std::string server_endpoint = argv[1];
  if(server_endpoint.find("://") == std::string::npos) {
    logging::ERROR("Usage: " + std::string(argv[0]) + " server_listen_endpoint");
    return 1;
  }

  //root dir
  if(argc > 2)
    root = argv[2];

  //change these to tcp://known.ip.address.with:port if you want to do this across machines
  zmq::context_t context;
  std::string result_endpoint = "ipc://result_endpoint";
  std::string request_interrupt = "ipc://request_interrupt";
  std::string proxy_endpoint = "ipc://proxy_endpoint";

  //server
  std::thread server = std::thread(std::bind(&http_server_t::serve,
    http_server_t(context, server_endpoint, proxy_endpoint + "_upstream", result_endpoint, request_interrupt, true)));

  //load balancer for file serving
  std::thread file_proxy(std::bind(&proxy_t::forward, proxy_t(context, proxy_endpoint + "_upstream", proxy_endpoint + "_downstream")));
  file_proxy.detach();

  //file serving thread
  std::thread file_worker(std::bind(&worker_t::work,
    worker_t(context, proxy_endpoint + "_downstream", "ipc:///dev/null", result_endpoint, request_interrupt,
    std::bind(&disk_work, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3)
  )));
  file_worker.detach();

  //listen for SIGINT and terminate if we hear it
  std::signal(SIGINT, [](int s){ std::this_thread::sleep_for(std::chrono::seconds(1)); exit(1); });
  server.join();


  return 0;
}

