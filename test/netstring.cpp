#include "testing/testing.hpp"
#include "prime_server.hpp"
#include "netstring_protocol.hpp"

#include <unistd.h>
#include <functional>
#include <memory>
#include <unordered_set>
#include <thread>
#include <iterator>
#include <cstdlib>
#include <cstring>

using namespace prime_server;

namespace {
  class testable_netstring_server_t : public netstring_server_t {
   public:
    using netstring_server_t::netstring_server_t;
    using netstring_server_t::enqueue;
    using netstring_server_t::request_id;
    //zmq is great, it will hold on to unsent messages so that if you are disconnected
    //and reconnect, they eventually do get sent, for this test we actually want them
    //dropped since we arent really testing their delivery here
    void passify() {
      int disabled = 0;
      proxy.setsockopt(ZMQ_LINGER, &disabled, sizeof(disabled));
    }
  };

  class testable_netstring_client_t : public netstring_client_t {
   public:
    using netstring_client_t::netstring_client_t;
    using netstring_client_t::stream_responses;
    using netstring_client_t::response;
  };

  void test_streaming_server() {
    zmq::context_t context;
    testable_netstring_server_t server(context, "ipc:///tmp/test_netstring_server", "ipc:///tmp/test_netstring_proxy_upstream", "ipc:///tmp/test_netstring_results");
    server.passify();

    netstring_entity_t request;
    std::string incoming("1");
    server.enqueue(static_cast<const void*>(incoming.data()), incoming.size(), "irgendjemand", request);
    incoming = "2:abgeschnitte,3:mer,5:welle,5:luege,5:oeb's,4:guet,4:isch,81:du_siehscht_mi_noed";
    server.enqueue(static_cast<const void*>(incoming.data()), incoming.size(), "irgendjemand", request);
    if(server.request_id != 7)
      throw std::runtime_error("Wrong number of requests were forwarded");
    if(request.body != "du_siehscht_mi_noed")
      throw std::runtime_error("Unexpected partial request data");
  }

  void test_streaming_client() {
    std::string all;
    size_t responses = 0;
    zmq::context_t context;
    testable_netstring_client_t client(context, "ipc:///tmp/test_netstring_server",
      [](){ return std::make_pair<void*, size_t>(nullptr, 0); },
      [&all, &responses](const void* data, size_t size){
        auto response = netstring_entity_t::from_string(static_cast<const char*>(data), size);
        all.append(response.body);
        ++responses;
        return true;
      });

    bool more;
    std::string incoming("1");
    auto reported_responses = client.stream_responses(static_cast<const void*>(incoming.data()), incoming.size(), more);
    incoming = "4:abgeschnitteni,11";
    reported_responses += client.stream_responses(static_cast<const void*>(incoming.data()), incoming.size(), more);
    incoming = ":rueckmeldig,3:mer,5:welle,5:luege,5:oeb's,4:guet,4:isch,81:du_siehscht_mi_noed";
    reported_responses += client.stream_responses(static_cast<const void*>(incoming.data()), incoming.size(), more);

    if(!more)
      throw std::runtime_error("Expected the client to want more responses");
    if(all != "abgeschnittenirueckmeldigmerwelleluegeoeb'sguetisch")
      throw std::runtime_error("Unexpected response data");
    if(responses != 8)
      throw std::runtime_error("Wrong number of responses were collected");
    if(reported_responses != responses)
      throw std::runtime_error("Wrong number of responses were reported");
    if(client.response.body != "du_siehscht_mi_noed" || client.response.body_length != 81)
      throw std::runtime_error("Unexpected partial response data");
  }

  void test_entity() {
    std::string body = "e_chliises_schtoeckli";
    std::string netstring = netstring_entity_t::to_string(body);
    if(netstring != "21:e_chliises_schtoeckli,")
      throw std::runtime_error("Request was not well-formed");
    if(netstring_entity_t::from_string(netstring.c_str(), netstring.size()).body != body)
      throw std::runtime_error("Request was not properly parsed");
  }

  constexpr char alpha_numeric[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

  std::string random_string(size_t length) {
    std::string random(length, '\0');
    for(auto& c : random)
      c = alpha_numeric[rand() % (sizeof(alpha_numeric) - 1)];
    return random;
  }

  void netstring_client_work(zmq::context_t& context) {
    //client makes requests and gets back responses in a batch fashion
    const size_t total = 100000;
    std::unordered_set<std::string> requests;
    size_t received = 0;
    std::string request;
    netstring_client_t client(context, "ipc:///tmp/test_netstring_server",
      [&requests, &request]() {
        //we want more requests
        if(requests.size() < total) {
          std::pair<std::unordered_set<std::string>::iterator, bool> inserted = std::make_pair(requests.end(), false);
          while(inserted.second == false) {
            request = random_string(50);
            inserted = requests.insert(request);
          }
          request = netstring_entity_t::to_string(request);
        }//blank request means we are done
        else
          request.clear();
        return std::make_pair(static_cast<const void*>(request.c_str()), request.size());
      },
      [&requests, &received](const void* data, size_t size) {
        //get the result and tell if there is more or not
        auto response = netstring_entity_t::from_string(static_cast<const char*>(data), size);
        if(requests.find(response.body) == requests.end())
          throw std::runtime_error("Unexpected response!");
        return ++received < total;
      }, 100
    );
    //request and receive
    client.batch();
  }

  constexpr size_t MAX_REQUEST_SIZE = 1024*1024;

  void test_parallel_clients() {

    zmq::context_t context;

    //server
    std::thread server(std::bind(&netstring_server_t::serve,
      netstring_server_t(context, "ipc:///tmp/test_netstring_server", "ipc:///tmp/test_netstring_proxy_upstream", "ipc:///tmp/test_netstring_results", false, MAX_REQUEST_SIZE)));
    server.detach();

    //load balancer for parsing
    std::thread proxy(std::bind(&proxy_t::forward,
      proxy_t(context, "ipc:///tmp/test_netstring_proxy_upstream", "ipc:///tmp/test_netstring_proxy_downstream")));
    proxy.detach();

    //echo worker
    std::thread worker(std::bind(&worker_t::work,
      worker_t(context, "ipc:///tmp/test_netstring_proxy_downstream", "ipc:///tmp/NONE", "ipc:///tmp/test_netstring_results",
      [] (const std::list<zmq::message_t>& job, void*) {
        worker_t::result_t result{false};
        auto request = netstring_entity_t::from_string(static_cast<const char*>(job.front().data()), job.front().size());
        auto response = netstring_entity_t::to_string(request.body);
        result.messages.emplace_back(response);
        return result;
      }
    )));
    worker.detach();

    //make a bunch of clients
    std::thread client1(std::bind(&netstring_client_work, context));
    std::thread client2(std::bind(&netstring_client_work, context));
    client1.join();
    client2.join();
  }

  void test_malformed() {
    zmq::context_t context;
    std::string request = "isch_doch_unsinn";
    netstring_client_t client(context, "ipc:///tmp/test_netstring_server",
      [&request]() {
        return std::make_pair(static_cast<const void*>(request.c_str()), request.size());
      },
      [](const void* data, size_t size) {
        auto response = netstring_entity_t::from_string(static_cast<const char*>(data), size);
        if(response.body.substr(0, 11) != "BAD_REQUEST")
          throw std::runtime_error("Expected BAD_REQUEST response!");
        return false;
      }, 1
    );
    client.batch();

    //TODO: check that you're disconnected
  }

  void test_too_large() {
    zmq::context_t context;
    std::string request = netstring_entity_t::to_string(std::string(MAX_REQUEST_SIZE + 10, '!'));
    netstring_client_t client(context, "ipc:///tmp/test_netstring_server",
      [&request]() {
        return std::make_pair(static_cast<const void*>(request.c_str()), request.size());
      },
      [](const void* data, size_t size) {
        auto response = netstring_entity_t::from_string(static_cast<const char*>(data), size);
        if(response.body.substr(0, 8) != "TOO_LONG")
          throw std::runtime_error("Expected TOO_LONG response!");
        return false;
      }, 1
    );
    client.batch();

    //TODO: check that you're disconnected
  }


  void test_large_request() {
    zmq::context_t context;

    //make a nice visible ascii string request
    std::string request(MAX_REQUEST_SIZE - 100, ' ');
    for(size_t i = 0; i < request.size(); ++i)
      request[i] = (i % 95) + 32;
    request = netstring_entity_t::to_string(request);

    //see if we get it back
    netstring_client_t client(context, "ipc:///tmp/test_netstring_server",
      [&request]() {
        return std::make_pair(static_cast<const void*>(request.c_str()), request.size());
      },
      [&request](const void* data, size_t size) {
        //get the result and tell if there is more or not
        if(size != request.size())
          throw std::runtime_error("Unexpected response size!");
        if(strcmp(static_cast<const char*>(data), request.c_str()) != 0)
          throw std::runtime_error("Unexpected response data!");
        return false;
      }, 1
    );
    //request and receive
    client.batch();
  }

}

int main() {
  //make this whole thing bail if it doesnt finish fast
  alarm(60);

  testing::suite suite("netstring");

  suite.test(TEST_CASE(test_streaming_client));

  suite.test(TEST_CASE(test_streaming_server));

  suite.test(TEST_CASE(test_entity));

  suite.test(TEST_CASE(test_parallel_clients));

  suite.test(TEST_CASE(test_malformed));

  suite.test(TEST_CASE(test_too_large));

  suite.test(TEST_CASE(test_large_request));

  return suite.tear_down();
}
