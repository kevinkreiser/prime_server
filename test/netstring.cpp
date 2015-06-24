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
    using netstring_client_t::buffer;
  };

  void test_streaming_server() {
    zmq::context_t context;
    testable_netstring_server_t server(context, "ipc://test_netstring_server", "ipc://test_netstring_proxy_upstream", "ipc://test_netstring_results");
    server.passify();

    std::string buffer;
    std::string incoming("1");
    server.enqueue(static_cast<const void*>(incoming.data()), incoming.size(), "irgendjemand", buffer);
    incoming = "2:abgeschnitte,3:mer,5:welle,5:luege,5:oeb's,4:guet,4:isch,du_siehscht_mi_noed";
    server.enqueue(static_cast<const void*>(incoming.data()), incoming.size(), "irgendjemand", buffer);
    if(server.request_id != 7)
      throw std::runtime_error("Wrong number of requests were forwarded");
    if(buffer != "du_siehscht_mi_noed")
      throw std::runtime_error("Unexpected partial request data");
  }

  void test_streaming_client() {
    std::string all;
    size_t responses = 0;
    zmq::context_t context;
    testable_netstring_client_t client(context, "ipc://test_netstring_server",
      [](){ return std::make_pair<void*, size_t>(nullptr, 0); },
      [&all, &responses](const void* data, size_t size){
        all.append(static_cast<const char*>(data), size);
        ++responses;
        return true;
      });

    bool more;
    std::string incoming("1");
    auto reported_responses = client.stream_responses(static_cast<const void*>(incoming.data()), incoming.size(), more);
    incoming = "4:abgeschnitteni,11";
    reported_responses += client.stream_responses(static_cast<const void*>(incoming.data()), incoming.size(), more);
    incoming = ":rueckmeldig,3:mer,5:welle,5:luege,5:oeb's,4:guet,4:isch,du_siehscht_mi_noed";
    reported_responses += client.stream_responses(static_cast<const void*>(incoming.data()), incoming.size(), more);

    if(!more)
      throw std::runtime_error("Expected the client to want more responses");
    if(all != "abgeschnittenirueckmeldigmerwelleluegeoeb'sguetisch")
      throw std::runtime_error("Unexpected response data");
    if(responses != 8)
      throw std::runtime_error("Wrong number of responses were collected");
    if(reported_responses != responses)
      throw std::runtime_error("Wrong number of responses were reported");
    if(client.buffer != "du_siehscht_mi_noed")
      throw std::runtime_error("Unexpected partial response data");
  }

  void test_request() {
    std::string netstring("e_chliises_schtoeckli");
    prime_server::netstring_request_t::format(netstring);
    if(netstring != "21:e_chliises_schtoeckli,")
      throw std::runtime_error("Request was not well-formed");
  }

  void test_response() {
    std::string netstring("e_chliises_schtoeckli");
    prime_server::netstring_request_t::format(netstring);
    if(netstring != "21:e_chliises_schtoeckli,")
      throw std::runtime_error("Response was not well-formed");
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
    netstring_client_t client(context, "ipc://test_netstring_server",
      [&requests, &request]() {
        //we want more requests
        if(requests.size() < total) {
          std::pair<std::unordered_set<std::string>::iterator, bool> inserted = std::make_pair(requests.end(), false);
          while(inserted.second == false) {
            request = random_string(10) + "eifach e chlii Zusatz fer de Nachricht groesser mache. mer welle de Grenz ueberschriite";
            inserted = requests.insert(request);
          }
          netstring_request_t::format(request);
        }//blank request means we are done
        else
          request.clear();
        return std::make_pair(static_cast<const void*>(request.c_str()), request.size());
      },
      [&requests, &received](const void* data, size_t size) {
        //get the result and tell if there is more or not
        std::string response(static_cast<const char*>(data), size);
        if(requests.find(response) == requests.end())
          throw std::runtime_error("Unexpected response!");
        return ++received < total;
      }, 100
    );
    //request and receive
    client.batch();
  }

  void test_parallel_clients() {

    zmq::context_t context;

    //server
    std::thread server(std::bind(&netstring_server_t::serve,
      netstring_server_t(context, "ipc://test_netstring_server", "ipc://test_netstring_proxy_upstream", "ipc://test_netstring_results")));
    server.detach();

    //load balancer for parsing
    std::thread proxy(std::bind(&proxy_t::forward,
      proxy_t(context, "ipc://test_netstring_proxy_upstream", "ipc://test_netstring_proxy_downstream")));
    proxy.detach();

    //echo worker
    std::thread worker(std::bind(&worker_t::work,
      worker_t(context, "ipc://test_netstring_proxy_downstream", "ipc://NONE", "ipc://test_netstring_results",
      [] (const std::list<zmq::message_t>& job, void*) {
        worker_t::result_t result{false};
        result.messages.emplace_back(static_cast<const char*>(job.front().data()), job.front().size());
        netstring_response_t::format(result.messages.back());
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

}

int main() {
  //make this whole thing bail if it doesnt finish fast
  alarm(30);

  testing::suite suite("netstring");

  suite.test(TEST_CASE(test_streaming_client));

  suite.test(TEST_CASE(test_streaming_server));

  suite.test(TEST_CASE(test_request));

  suite.test(TEST_CASE(test_response));

  suite.test(TEST_CASE(test_parallel_clients));

  return suite.tear_down();
}
