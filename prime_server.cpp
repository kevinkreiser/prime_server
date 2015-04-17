#include <zmq.hpp>
#include <thread>
#include <functional>
#include <memory>
#include <string>
#include <list>
#include <set>
#include <iostream>
#include <unordered_set>

#include "messaging.hpp"
using namespace messaging;

/*
 * this system is essentially just a server or a simulated one that tells you
 * whether or not a given input number is prime. the aim isn't really to do any
 * type of novel large prime computation but rather to contrive a system whose
 * units of work are highly non-uniform in terms of their time to completion.
 * this is a common problem in many other workflows an primes seemed like a
 * good way to illustrate this.
 *
 * the system we are looking to build is something like the following:

                                        ==========                   ==========
                                        | worker |                   | worker |
                                        | worker |                   | worker |
   client <---> server ---> proxy <---> |  ....  | <---> proxy <---> |  ....  | <---> ...
                  ^                     | worker |                   | worker |
                  |                     | worker |                   | worker |
                  |                     ==========                   ==========
                  |                         |                            |
                  |_________________________|____________________________|___________ ...

 * a client (a browser or just a thread within this process) makes request to a server
 * the server listens for new requests and replies when the backend bits send back results
 * the backend is comprised of load balancing proxies between layers of worker pools
 * in real life you may run these in different processes or on different machines
 * we just use threads to simulate it, ie. no classic mutex patterns to worry about
 *
 * so this system lets you handle one type of request that can decomposed into multiple steps
 * that is useful if certain steps take longer than others because you can scale them individually
 * it doesn't really handle multiple types of requests unless workers learn more than one job
 * to fix this we could upgrade the workers to be able to forward work to more than one proxy
 * this would allow heterogeneous workflows without having making larger pluripotent workers
 * and therefore would allow scaling of various workflows independently of each other
 * an easier approach would be just running a separate cluster per workflow, pros and cons there
 */


int main(int argc, char** argv) {

  //number of jobs to do
  size_t requests = 10;
  if(argc > 1)
    requests = std::stoul(argv[1]);

  //number of workers to use at each stage
  size_t worker_concurrency = 1;
  if(argc > 2)
    worker_concurrency = std::stoul(argv[2]);

  //change these to tcp://known.ip.address.with:port if you want to do this across machines
  std::shared_ptr<zmq::context_t> context_ptr = std::make_shared<zmq::context_t>(1);
  std::string result_endpoint = "ipc://result_endpoint";
  std::string parse_proxy_endpoint = "ipc://parse_proxy_endpoint";
  std::string compute_proxy_endpoint = "ipc://compute_proxy_endpoint";

  //see if you want to actually serve http requests or not
  std::string server_endpoint = "ipc://server_endpoint";
  bool real_server = false;
  if(argc > 3) {
    server_endpoint = argv[3];
    real_server = true;
  }

  //server
  std::thread server_thread(std::bind(&server_t::serve, server_t(context_ptr, server_endpoint, parse_proxy_endpoint + "_upstream", result_endpoint)));

  //load balancer for parsing
  std::thread parse_proxy(std::bind(&proxy_t::forward, proxy_t(context_ptr, parse_proxy_endpoint + "_upstream", parse_proxy_endpoint + "_downstream")));
  parse_proxy.detach();

  //request parsers
  std::list<std::thread> parse_worker_threads;
  for(size_t i = 0; i < worker_concurrency; ++i) {
    parse_worker_threads.emplace_back(std::bind(&worker_t::work,
      worker_t(context_ptr, parse_proxy_endpoint + "downstream", compute_proxy_endpoint + "_upstream", result_endpoint,
      [] (const std::list<zmq::message_t>& job) {
        //parse the string into a size_t
        worker_t::result_t result{true};
        result.messages.emplace_back(sizeof(size_t));
        const size_t possible_prime = std::stoul(std::string(static_cast<const char*>(job.front().data()), job.front().size()));
        *static_cast<size_t*>(result.messages.back().data()) = possible_prime;
        return result;
      }
    )));
    parse_worker_threads.back().detach();
  }

  //load balancer for prime computation
  std::thread compute_proxy(std::bind(&proxy_t::forward, proxy_t(context_ptr, compute_proxy_endpoint + "_upstream", compute_proxy_endpoint + "_downstream")));
  compute_proxy.detach();

  //prime computers
  std::list<std::thread> compute_worker_threads;
  for(size_t i = 0; i < worker_concurrency; ++i) {
    compute_worker_threads.emplace_back(std::bind(&worker_t::work,
      worker_t(context_ptr, compute_proxy_endpoint + "downstream", "ipc://NO_ENDPOINT", result_endpoint,
      [] (const std::list<zmq::message_t>& job) {
        //check if its prime
        const size_t prime = *static_cast<const size_t*>(job.front().data());
        size_t divisor = 2;
        size_t high = prime;
        while(divisor < high) {
          if(prime % divisor == 0)
            break;
          high = prime / divisor;
          ++divisor;
        }

        //if it was prime send it back unmolested, else send back 2 which we know is prime
        worker_t::result_t result{false};
        result.messages.emplace_back(sizeof(size_t));
        *static_cast<size_t*>(result.messages.back().data()) = (divisor >= high ? prime : static_cast<size_t>(2));
        return result;
      }
    )));
    compute_worker_threads.back().detach();
  }

  //listen for requests from some other client indefinitely
  if(real_server) {
    server_thread.join();
    //TODO: should we listen for SIGINT and terminate gracefully/exit(0)?
    return 0;
  }//or make a client in process and quit when its batch is done
  else
    server_thread.detach();

  //client makes requests and gets back responses in a batch fashion
  size_t produced_requests = 0, collected_results;
  std::set<size_t> primes = {2};
  client_t client(context_ptr, server_endpoint,
    [requests, &produced_requests]() {
      std::list<zmq::message_t> messages;
      if(produced_requests != requests)
      {
        auto request = std::to_string(produced_requests * 2 + 3);
        messages.emplace_back(request.size());
        std::copy(request.begin(), request.end(), static_cast<char*>(messages.back().data()));
        ++produced_requests;
      }
      return messages;
    },
    [requests, &primes, &collected_results] (const std::list<zmq::message_t>& result) {
        primes.insert(*static_cast<const size_t*>(result.front().data()));
        ++collected_results;
        return collected_results == requests;
      }
  );
  //make the requests
  client.request();
  //get back the responses
  client.collect();
  //show primes
  //for(const auto& prime : primes)
  //  std::cout << prime << " | ";
  std::cout << primes.size() << std::endl;

  return 0;
}
