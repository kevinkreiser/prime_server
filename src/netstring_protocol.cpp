#include "netstring_protocol.hpp"
#include "logging.hpp"

#include <cctype>
#include <cstdlib>
#include <ctime>

namespace {

  void log_transaction(uint64_t id, const std::string& transaction) {
    auto log_line = std::to_string(id);
    log_line.push_back(' ');
    log_line += logging::timestamp();
    log_line.push_back(' ');
    log_line += transaction;
    log_line.push_back('\n');
    logging::log(log_line);
  }

  const std::string INTERNAL_ERROR(prime_server::netstring_entity_t::to_string("INTERNAL_ERROR: empty response"));

}

namespace prime_server {

  netstring_entity_t::netstring_entity_t():body(),body_length(0) {
  }

  std::string netstring_entity_t::to_string() const {
    return std::to_string(body.size()) + ':' + body + ',';
  }

  std::string netstring_entity_t::to_string(const std::string& body) {
    return std::to_string(body.size()) + ':' + body + ',';
  }

  netstring_entity_t netstring_entity_t::from_string(const char* start, size_t length) {
    netstring_entity_t request;
    auto requests = request.from_stream(start, length);
    if(requests.size() == 0)
      throw std::runtime_error("Incomplete netstring request");
    return std::move(requests.front());
  }

  std::list<netstring_entity_t> netstring_entity_t::from_stream(const char* start, size_t length, size_t max_size) {
    std::list<netstring_entity_t> requests;
    size_t i = 0;
    size_t remaining = 0;
    while(i < length) {
      //we are looking for the length
      if(body_length == 0) {
        //if its not a digit then we need to see what it is
        auto c = start[i];
        if(!std::isdigit(c)){
          //found the end of the length
          if(c == ':') {
            //get the length
            body.append(start, start + i);
            try { body_length = std::stoul(body); }
            catch(...) { throw std::runtime_error("BAD_REQUEST: Non-numeric length"); }
            if(body_length > max_size)
              throw std::runtime_error("TOO_LONG: Request exceeded max");
            //reset for body
            start += i + 1;
            length -= i + 1;
            i = 0;
            body.clear();
            continue;
          }//this isnt what we expect
          else
            throw std::runtime_error("BAD_REQUEST: Missing ':' between length and body");
        }
        //next char
        ++i;
        continue;
      }//we are looking for the body
      else if((remaining = body_length - body.size()) < length) {
        if(start[remaining] == ',') {
          //make a new one
          requests.emplace_back();
          requests.back().body = std::move(body);
          requests.back().body_length = 0;
          requests.back().body.append(start, start + remaining);
          //reset for next
          start += remaining + 1;
          length -= remaining + 1;
          i = 0;
          flush_stream();
          continue;
        }//this isnt what we expect
        else
          throw std::runtime_error("BAD_REQUEST: Missing ',' after body");
      }
      //we werent looking for length, and the body didnt fit in what we had
      //so we are done for now and need more data
      break;
    }
    //whatever is left we want to keep, it could be nothing
    body.append(start, start + length);
    return requests;
  }

  void netstring_entity_t::flush_stream() {
    body.clear();
    body_length = 0;
  }

  size_t netstring_entity_t::size() const {
    //TODO: pedantic, comma, colon and length are part of the request
    return body.size();
  }

  size_t netstring_client_t::stream_responses(const void* message, size_t size, bool& more) {
    auto responses = response.from_stream(static_cast<const char*>(message), size);
    for(const auto& parsed_response : responses) {
      //TODO: this is wasteful
      auto formatted_response = netstring_entity_t::to_string(parsed_response.body);
      more = collect_function(static_cast<const void *>(formatted_response.c_str()), formatted_response.size());
    }
    return responses.size();
  }

  netstring_server_t::netstring_server_t(zmq::context_t& context, const std::string& client_endpoint, const std::string& proxy_endpoint,
                                         const std::string& result_endpoint, const std::string& interrupt_endpoint, bool log, size_t max_request_size):
                                         server_t<netstring_entity_t, netstring_request_info_t>::server_t(context, client_endpoint, proxy_endpoint,
                                         result_endpoint, interrupt_endpoint, log, max_request_size), request_id(0) {
  }

  netstring_server_t::~netstring_server_t(){}

  bool netstring_server_t::enqueue(const zmq::message_t& requester, const zmq::message_t& message, netstring_entity_t& request) {
    //do some parsing
    std::list<netstring_entity_t> parsed_requests;
    try {
      parsed_requests = request.from_stream(static_cast<const char*>(message.data()), message.size(), max_request_size);
    }//something went wrong either bad request or too long
    catch(const std::runtime_error& e) {
      client.send(requester, ZMQ_SNDMORE | ZMQ_DONTWAIT);
      client.send(netstring_entity_t::to_string(e.what()), ZMQ_DONTWAIT);
      if(log) {
        log_transaction(request_id, e.what());
        log_transaction(request_id, "REPLIED");
      }
      ++request_id;
      return true;
    }

    //send on each request
    for(const auto& parsed_request : parsed_requests) {
      netstring_request_info_t info{request_id, static_cast<uint32_t>(difftime(time(nullptr), static_cast<time_t>(0)) + .5)};
      this->proxy.send(static_cast<const void*>(&info), sizeof(netstring_request_info_t), ZMQ_DONTWAIT | ZMQ_SNDMORE);
      this->proxy.send(parsed_request.to_string(), ZMQ_DONTWAIT);
      if(log)
        log_transaction(request_id, request.body);
      //remember we are working on it
      this->requests.emplace(request_id++, requester);
    }
    return true;
  }

  void netstring_server_t::dequeue(const std::list<zmq::message_t>& messages) {
    //find the request
    const auto& request_info = *static_cast<const uint64_t*>(messages.front().data());
    auto request = requests.find(request_info);
    if(request == requests.end()) {
      logging::WARN("Unknown or timed-out request id: " + std::to_string(request_info));
      return;
    }
    //reply to the client with the response or an error
    client.send(request->second, ZMQ_SNDMORE | ZMQ_DONTWAIT);
    if(messages.size() == 2)
      client.send(messages.back(), ZMQ_DONTWAIT);
    else
      client.send(INTERNAL_ERROR, ZMQ_DONTWAIT);
    if(log)
      log_transaction(request_info, "REPLIED");
    //cleanup, but leave the session as netstring is always keep alive
    requests.erase(request);
  }

}
