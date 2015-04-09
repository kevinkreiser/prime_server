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
  const char* requests_in = "ipc://requests_in";
  const char* parsed_in = "ipc://parsed_in";
  const char* primes_in = "ipc://primes_in";
  const char* primes_out = "ipc://primes_out";

  //request producer
  size_t produced_requests = 0;
  producer request_producer(context_ptr, requests_in,
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
    }
  );

  //request parsers
  std::list<std::thread> parse_worker_threads;
  for(size_t i = 0; i < worker_concurrency; ++i) {
    parse_worker_threads.emplace_back(std::bind(&worker::work, worker(context_ptr, requests_in, parsed_in,
      [] (const std::list<zmq::message_t>& job) {
        //parse the string into a size_t
        std::list<zmq::message_t> messages;
        messages.emplace_back(sizeof(size_t));
        const size_t possible_prime = std::stoul(std::string(static_cast<const char*>(job.front().data()), job.front().size()));
        *static_cast<size_t*>(messages.back().data()) = possible_prime;
        return messages;
      }
    )));
    parse_worker_threads.back().detach();
  }

  //router from parsed requests to prime computation workers
  std::thread prime_router_thread(std::bind(&router::route, router(context_ptr, parsed_in, primes_in)));
  prime_router_thread.detach();

  //prime computers
  std::list<std::thread> prime_worker_threads;
  for(size_t i = 0; i < worker_concurrency; ++i) {
    prime_worker_threads.emplace_back(std::bind(&worker::work, worker(context_ptr, primes_in, primes_out,
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
        std::list<zmq::message_t> messages;
        messages.emplace_back(sizeof(size_t));
        *static_cast<size_t*>(messages.back().data()) = (divisor >= high ? prime : static_cast<size_t>(2));
        return messages;
      }
    )));
    prime_worker_threads.back().detach();
  }

  //result collector
  std::set<size_t> primes = {2};
  size_t collected_results = 0;
  std::thread collector_thread(std::bind(&collector::collect, collector(context_ptr, primes_out,
    [requests, &primes, &collected_results] (const std::list<zmq::message_t>& result) {
      primes.insert(*static_cast<const size_t*>(result.front().data()));
      ++collected_results;
      return collected_results == requests;
    }
  )));

  //started last so we don't miss requests from it
  std::thread producer_thread(std::bind(&producer::produce, &request_producer));

  //wait for the collector to get all the jobs
  collector_thread.join();
  producer_thread.join();

  //show primes
  //for(const auto& prime : primes)
  //  std::cout << prime << " | ";
  std::cout << primes.size() << std::endl;

  return 0;
}
