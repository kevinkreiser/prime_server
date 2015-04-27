#include "testing/testing.hpp"
#include "prime_server.hpp"
#include "protocols.hpp"

#include <functional>
#include <memory>
#include <unordered_set>
#include <thread>
#include <iterator>
#include <cstdlib>
#include <cstring>

using namespace prime_server;

namespace {
  constexpr char alpha_numeric[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

  std::string random_string(size_t length) {
    std::string random(length, '\0');
    for(auto& c : random)
      c = alpha_numeric[rand() % (sizeof(alpha_numeric) - 1)];
    return random;
  }

  void test_separate() {
    std::string http("GET /irgendwelle/pfad HTTP1.1\r\n\r\nGET /annrer/pfad?aafrag=gel HTTP1.0\r\n\r\nsell siehscht du au noed");
    size_t consumed;
    auto parts = prime_server::http_protocol_t::separate(static_cast<const void*>(http.data()), http.size(), consumed);
    if(parts.size() != 2)
      throw std::runtime_error("Wrong number of parts when separated");
    if(http.substr(consumed) != "sell siehscht du au noed")
      throw std::runtime_error("Didn't consume the right amount of the string");
    auto itr = parts.begin();
    std::advance(itr, 1);
    std::string x(static_cast<const char*>(itr->first), itr->second);
    if(std::string(static_cast<const char*>(itr->first), itr->second) != "GET /annrer/pfad?aafrag=gel HTTP1.0\r\n\r\n")
      throw std::runtime_error("Wrong part");
  }

  void test_delineate() {
    std::string http("e_chliises_schtoeckli");
    auto message = prime_server::http_protocol_t::delineate(static_cast<const void*>(http.data()), http.size());
    std::string delineated(static_cast<const char*>(message.data()), message.size());
    if(delineated != "GET e_chliises_schtoeckli HTTP/1.1\r\n\r\n")
      throw std::runtime_error("Message was not properly delineated");
  }

  void netstring_client_work(std::shared_ptr<zmq::context_t>& context_ptr) {
    //client makes requests and gets back responses in a batch fashion
    const size_t total = 100000;
    std::unordered_set<std::string> requests;
    size_t received = 0;
    std::string request;
    client_t<netstring_protocol_t> client(context_ptr, "ipc://test_http_server",
      [&requests, &request]() {
        //we want 10k requests
        if(requests.size() < total) {
          std::pair<std::unordered_set<std::string>::iterator, bool> inserted = std::make_pair(requests.end(), false);
          while(inserted.second == false) {
            request = random_string(10);
            inserted = requests.insert(request);
          }
        }//blank request means we are done
        else
          request.clear();
        return std::make_pair(static_cast<const void*>(request.c_str()), request.size());
      },
      [&requests, &received] (const std::pair<const void*, size_t>& result) {
        //get the result and tell if there is more or not
        std::string response(static_cast<const char*>(result.first), result.second);
        if(requests.find(response) == requests.end())
          throw std::runtime_error("Unexpected response!");
        return ++received < total;
      }, 100
    );
    //request and receive
    client.batch();
  }

  void test_parallel_clients() {

    auto context_ptr = std::make_shared<zmq::context_t>(1);

    //server
    std::thread server(std::bind(&server_t<http_protocol_t>::serve,
     server_t<http_protocol_t>(context_ptr, "ipc://test_http_server", "ipc://test_http_proxy_upstream", "ipc://test_http_results")));
    server.detach();

    //load balancer for parsing
    std::thread proxy(std::bind(&proxy_t::forward,
      proxy_t(context_ptr, "ipc://test_http_proxy_upstream", "ipc://test_http_proxy_downstream")));
    proxy.detach();

    //echo worker
    std::thread worker(std::bind(&worker_t::work,
      worker_t(context_ptr, "ipc://test_http_proxy_downstream", "ipc://NONE", "ipc://test_http_results",
      [] (const std::list<zmq::message_t>& job) {
        worker_t::result_t result{false};
        result.messages.emplace_back(job.front().size());
        memcpy(result.messages.back().data(), job.front().data(), job.front().size());
        return result;
      }
    )));
    worker.detach();

    //make a bunch of clients
    std::thread client1(std::bind(&netstring_client_work, context_ptr));
    std::thread client2(std::bind(&netstring_client_work, context_ptr));
    client1.join();
    client2.join();
  }

}

int main() {
  testing::suite suite("netstring");

  suite.test(TEST_CASE(test_separate));

  suite.test(TEST_CASE(test_delineate));

  suite.test(TEST_CASE(test_parallel_clients));

  return suite.tear_down();
}
