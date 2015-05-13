#include "netstring_protocol.hpp"
#include "logging.hpp"

#include <cctype>

namespace prime_server {

  size_t netstring_client_t::stream_responses(const void* message, size_t size, bool& more) {
    //um..
    size_t collected = 0;
    if(size == 0)
      return collected;

    //box out the message a bit so we can iterate
    const char* begin = static_cast<const char*>(message);
    const char* end = begin + size;

    //do we have a partial response in the buffer
    if(buffer.size()) {
      //make sure the buffer has a colon in it and bail otherwise
      auto pos = buffer.find(':');
      if(pos == std::string::npos) {
        while(begin < end) {
          buffer.push_back(*begin);
          ++begin;
          if(buffer.back() == ':')
            break;
        }
        if(begin == end)
          return collected;
        pos = buffer.size() - 1;
      }

      //copy up to what the message calls for, add one for the comma
      auto length = std::stoul(buffer.substr(0, pos)) + 1;
      length -= buffer.size() - (pos + 1);

      //not enough
      if(length > end - begin) {
        buffer.append(begin, end - begin);
        return collected;
      }//we have a full message here
      else {
        buffer.append(begin, length);
        more = collect_function(static_cast<const void *>(&buffer[pos + 1]), length - 1);
        ++collected;
        begin += length;
      }
    }

    //keep getting pieces if there is enough space for the message length plus a message
    if(begin < end && !isdigit(*begin))
      throw std::runtime_error("Netstring protocol message cannot begin with anything but a digit");
    const char* delim = begin;
    while(delim < end) {
      //get next colon
      const char* next_delim = delim;
      for(;next_delim < end; ++next_delim)
        if(*next_delim == ':')
          break;
      if(next_delim == end)
        break;

      //convert the previous portion to a number
      std::string length_str(delim, next_delim - delim);
      size_t length = std::stoul(length_str);
      const char* piece = next_delim + 1;
      next_delim += length + 2;

      //data is past the end which means we dont have it all yet
      if(next_delim > end)
        break;

      //tell where this piece is
      more = collect_function(static_cast<const void*>(piece), length);
      ++collected;
      delim = next_delim;
    }
    buffer.assign(delim, end - delim);
    return collected;
  }

  netstring_server_t::netstring_server_t(zmq::context_t& context, const std::string& client_endpoint, const std::string& proxy_endpoint, const std::string& result_endpoint, bool log):
    server_t<std::string, uint64_t>::server_t(context, client_endpoint, proxy_endpoint, result_endpoint, log), request_id(0) {
  }
  netstring_server_t::~netstring_server_t(){}
  void netstring_server_t::enqueue(const void* message, size_t size, const std::string& requester, std::string& buffer) {
    //box out the message a bit so we can iterate
    const char* begin = static_cast<const char*>(message);
    const char* end = begin + size;

    //do we have a partial request
    if(buffer.size()) {
      //make sure the buffer has a colon in it and bail otherwise
      auto pos = buffer.find(':');
      if(pos == std::string::npos) {
        while(begin < end) {
          buffer.push_back(*begin);
          ++begin;
          if(buffer.back() == ':')
            break;
        }
        if(begin == end)
          return;
        pos = buffer.size() - 1;
      }

      //copy up to what the message calls for, add one for the comma
      auto length = std::stoul(buffer.substr(0, pos)) + 1;
      length -= buffer.size() - (pos + 1);

      //not enough
      if(length > end - begin) {
        buffer.append(begin, end - begin);
        return;
      }//we have a full message here
      else {
        buffer.append(begin, length);
        //send on the request
        this->proxy.send(requester, ZMQ_DONTWAIT | ZMQ_SNDMORE);
        this->proxy.send(static_cast<void*>(&request_id), sizeof(request_id), ZMQ_DONTWAIT | ZMQ_SNDMORE);
        this->proxy.send(static_cast<void *>(&buffer[pos + 1]), length - 1, ZMQ_DONTWAIT);
        if(log) {
          std::string log_line = std::to_string(request_id);
          log_line.push_back(' ');
          log_line.append(&buffer[pos + 1], length - 1);
          LOG_INFO(log_line);
        }
        //remember we are working on it
        this->requests.emplace(request_id++, requester);
        begin += length;
      }
    }

    //keep getting pieces if there is enough space for the message length plus a message
    if(begin < end && !isdigit(*begin))
      throw std::runtime_error("Netstring protocol message cannot begin with anything but a digit");
    const char* delim = begin;
    while(delim < end) {
      //get next colon
      const char* next_delim = delim;
      for(;next_delim < end; ++next_delim)
        if(*next_delim == ':')
          break;
      if(next_delim == end)
        break;

      //convert the previous portion to a number
      std::string length_str(delim, next_delim - delim);
      size_t length = std::stoul(length_str);
      const char* piece = next_delim + 1;
      next_delim += length + 2;

      //data is past the end which means we dont have it all yet
      if(next_delim > end)
        break;

      //send on the request
      this->proxy.send(requester, ZMQ_DONTWAIT | ZMQ_SNDMORE);
      this->proxy.send(static_cast<void*>(&request_id), sizeof(request_id), ZMQ_DONTWAIT | ZMQ_SNDMORE);
      this->proxy.send(static_cast<const void*>(piece), length, ZMQ_DONTWAIT);
      if(log) {
        std::string log_line = std::to_string(request_id);
        log_line.push_back(' ');
        log_line.append(piece, length);
        LOG_INFO(log_line);
      }
      //remember we are working on it
      this->requests.emplace(request_id++, requester);
      delim = next_delim;
    }
    buffer.assign(delim, end - delim);
  }
  void netstring_server_t::dequeue(const uint64_t& request_info, size_t length) {
    auto removed = requests.erase(request_info);
    if(removed != 1)
      LOG_WARN("Unknown or timed-out request id: " + std::to_string(request_info));
    if(log)
      LOG_INFO(std::to_string(request_info) + " REPLIED");
  }

  void netstring_request_t::format(std::string& message) {
    message = std::to_string(message.size()) + ':' + message + ',';
  }

  void netstring_response_t::format(std::string& message) {
    message = std::to_string(message.size()) + ':' + message + ',';
  }

}
