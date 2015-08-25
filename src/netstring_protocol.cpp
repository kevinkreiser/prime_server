#include "netstring_protocol.hpp"
#include "logging.hpp"

#include <cctype>
#include <cstdlib>

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

  std::list<netstring_entity_t> netstring_entity_t::from_stream(const char* start, size_t length) {
    std::list<netstring_entity_t> requests;
    size_t i = 0;
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
            body_length = std::stoul(body);
            //reset for body
            start += i + 1;
            length -= i + 1;
            i = 0;
            body.clear();
            continue;
          }//this isnt what we expect
          else
            throw std::runtime_error("Malformed netstring request");
        }
        //next char
        ++i;
        continue;
      }//we are looking for the body
      else if(body_length - body.size() < length) {
        if(start[body_length - body.size()] == ',') {
          //make a new one
          requests.emplace_back();
          requests.back().body = std::move(body);
          requests.back().body.append(start, start + (body_length - body.size()));
          requests.back().body_length = body_length;
          //reset for next
          start += (body_length - body.size()) + 1;
          length -= (body_length - body.size()) + 1;
          i = 0;
          body_length = 0;
          body.clear();
          continue;
        }//this isnt what we expect
        else
          throw std::runtime_error("Malformed netstring request");
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

  netstring_server_t::netstring_server_t(zmq::context_t& context, const std::string& client_endpoint, const std::string& proxy_endpoint, const std::string& result_endpoint, bool log, size_t max_request_size):
    server_t<netstring_entity_t, uint64_t>::server_t(context, client_endpoint, proxy_endpoint, result_endpoint, log, max_request_size), request_id(0) {
  }

  netstring_server_t::~netstring_server_t(){}

  void netstring_server_t::enqueue(const void* message, size_t size, const std::string& requester, netstring_entity_t& request) {
    auto requests = request.from_stream(static_cast<const char*>(message), size);
    for(const auto& parsed_request : requests) {
      //send on the request
      this->proxy.send(requester, ZMQ_DONTWAIT | ZMQ_SNDMORE);
      this->proxy.send(static_cast<const void*>(&request_id), sizeof(request_id), ZMQ_DONTWAIT | ZMQ_SNDMORE);
      this->proxy.send(parsed_request.to_string(), ZMQ_DONTWAIT);
      if(log) {
        auto log_line = std::to_string(request_id);
        log_line.push_back(' ');
        log_line += logging::timestamp();
        log_line.push_back(' ');
        log_line += request.body;
        log_line.push_back('\n');
        logging::log(log_line);
      }
      //remember we are working on it
      this->requests.emplace(request_id++, requester);
    }
  }

  void netstring_server_t::dequeue(const uint64_t& request_info, size_t length) {
    //NOTE: netstring protocol is always keep alive so we leave the session intact
    auto removed = requests.erase(request_info);
    if(removed != 1)
      LOG_WARN("Unknown or timed-out request id: " + std::to_string(request_info));
    else if(log)
      LOG_INFO(std::to_string(request_info) + " REPLIED");
  }

}
