#include <thread>
#include <functional>
#include <string>
#include <list>
#include <set>
#include <iostream>
#include <unordered_set>
#include <memory>
#include <stdexcept>

//netstrings are far easier to work with but http is a more interesting use-case
//so we just do everthing using the http protocol here
#include "prime_server.hpp"
#include "http_protocol.hpp"
using namespace prime_server;
#include "logging.hpp"

int main(int argc, char** argv) {

  if(argc < 2) {
    LOG_ERROR("Usage: " + std::string(argv[0]) + " num_requests|server_listen_endpoint concurrency");
    return 1;
  }

  //number of jobs to do or server endpoint
  size_t requests = 0;
  std::string server_endpoint = "ipc://server_endpoint";
  if(std::string(argv[1]).find("://") != std::string::npos)
    server_endpoint = argv[1];
  else
    requests = std::stoul(argv[1]);

  //number of workers to use at each stage
  size_t worker_concurrency = 1;
  if(argc > 2)
    worker_concurrency = std::stoul(argv[2]);

  //change these to tcp://known.ip.address.with:port if you want to do this across machines
  zmq::context_t context;
  std::string result_endpoint = "ipc://result_endpoint";
  std::string parse_proxy_endpoint = "ipc://parse_proxy_endpoint";
  std::string compute_proxy_endpoint = "ipc://compute_proxy_endpoint";

  //server
  std::thread server_thread = std::thread(std::bind(&http_server_t::serve,
    http_server_t(context, server_endpoint, parse_proxy_endpoint + "_upstream", result_endpoint, requests == 0)));

  //load balancer for parsing
  std::thread parse_proxy(std::bind(&proxy_t::forward, proxy_t(context, parse_proxy_endpoint + "_upstream", parse_proxy_endpoint + "_downstream")));
  parse_proxy.detach();

  //request parsers
  std::list<std::thread> parse_worker_threads;
  for(size_t i = 0; i < worker_concurrency; ++i) {
    parse_worker_threads.emplace_back(std::bind(&worker_t::work,
      worker_t(context, parse_proxy_endpoint + "_downstream", compute_proxy_endpoint + "_upstream", result_endpoint,
      [] (const std::list<zmq::message_t>& job, void* request_info) {
        //request should look like
        ///is_prime?possible_prime=SOME_NUMBER
        try{
          auto request = http_request_t::from_string(static_cast<const char*>(job.front().data()), job.front().size());
          query_t::const_iterator prime_str;
          if(request.path != "/is_prime" || (prime_str = request.query.find("possible_prime")) == request.query.cend() || prime_str->second.size() != 1)
            throw std::runtime_error("");
          size_t possible_prime = std::stoul(prime_str->second.front());
          worker_t::result_t result{true};
          result.messages.emplace_back(static_cast<const char*>(static_cast<const void*>(&possible_prime)), sizeof(size_t));
          return result;
        }
        catch(...) {
          worker_t::result_t result{false};
          http_response_t response(400, "Bad Request");
          response.from_info(*static_cast<http_request_t::info_t*>(request_info));
          result.messages.emplace_back(response.to_string());
          return result;
        }
      }
    )));
    parse_worker_threads.back().detach();
  }

  //load balancer for prime computation
  std::thread compute_proxy(std::bind(&proxy_t::forward, proxy_t(context, compute_proxy_endpoint + "_upstream", compute_proxy_endpoint + "_downstream")));
  compute_proxy.detach();

  //prime computers
  std::list<std::thread> compute_worker_threads;
  for(size_t i = 0; i < worker_concurrency; ++i) {
    compute_worker_threads.emplace_back(std::bind(&worker_t::work,
      worker_t(context, compute_proxy_endpoint + "_downstream", "ipc://NO_ENDPOINT", result_endpoint,
      [] (const std::list<zmq::message_t>& job, void* request_info) {
        //check if its prime
        size_t prime = *static_cast<const size_t*>(job.front().data());
        size_t divisor = 2;
        size_t high = prime;
        while(divisor < high) {
          if(prime % divisor == 0)
            break;
          high = prime / divisor;
          ++divisor;
        }

        //if it was prime send it back unmolested, else send back 2 which we know is prime
        if(divisor < high)
          prime = 2;
        http_response_t response(200, "OK", std::to_string(prime));
        response.from_info(*static_cast<http_request_t::info_t*>(request_info));
        worker_t::result_t result{false};
        result.messages.emplace_back(response.to_string());
        return result;
      }
    )));
    compute_worker_threads.back().detach();
  }

  //make a client in process and quit when its batch is done
  //listen for requests from some other client indefinitely
  if(requests > 0) {
    server_thread.detach();
    //sometimes you miss getting results back because the sub socket
    //on the server hasnt yet connected with pub sockets on the workers
    //std::this_thread::sleep_for(std::chrono::seconds(1));

    //client makes requests and gets back responses in a batch fashion
    size_t produced_requests = 0, collected_results = 0;
    std::string request;
    std::set<size_t> primes = {2};
    http_client_t client(context, server_endpoint,
      [&request, requests, &produced_requests]() {
        //blank request means we are done
        if(produced_requests < requests)
          request = http_request_t::to_string(method_t::GET, "/is_prime?possible_prime=" + std::to_string(produced_requests++ * 2 + 3));
        else
          request.clear();
        return std::make_pair(static_cast<const void*>(request.c_str()), request.size());
      },
      [requests, &primes, &collected_results] (const void* message, size_t length) {
        //get the result and tell if there is more or not
        std::string response_str(static_cast<const char*>(message), length - 4);
        try {
          size_t number = std::stoul(response_str.substr(response_str.rfind('\n')));
          primes.insert(number);
        }
        catch(...) {
          LOG_ERROR("Responded with: " + response_str);
        }
        return ++collected_results < requests;
      }
    );
    //request and receive
    client.batch();
    //show primes
    //for(const auto& prime : primes)
    //  std::cout << prime << " | ";
    std::cout << primes.size() << std::endl;

  }//or listen for requests from some other client indefinitely
  else {
    server_thread.join();
    //TODO: should we listen for SIGINT and terminate gracefully/exit(0)?
  }

  return 0;
}
