#ifndef __NETSTRING_PROTOCOL_HPP__
#define __NETSTRING_PROTOCOL_HPP__

#include "prime_server.hpp"

#include <cctype>

namespace prime_server {

  class netstring_client_t : public client_t {
   public:
    using client_t::client_t;
   protected:
    virtual size_t stream_responses(const void* message, size_t size, bool& more) {
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
    std::string buffer;
  };

  class netstring_server_t : public server_t<std::string> {
   public:
    using server_t<std::string>::server_t;
    virtual ~netstring_server_t(){}
   protected:
    virtual size_t stream_requests(const void* message, size_t size, const std::string& requester, std::string& buffer) {
      //um..
      size_t forwarded = 0;
      if(size == 0)
        return forwarded;

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
            return forwarded;
          pos = buffer.size() - 1;
        }

        //copy up to what the message calls for, add one for the comma
        auto length = std::stoul(buffer.substr(0, pos)) + 1;
        length -= buffer.size() - (pos + 1);

        //not enough
        if(length > end - begin) {
          buffer.append(begin, end - begin);
          return forwarded;
        }//we have a full message here
        else {
          buffer.append(begin, length);
          this->proxy.send(requester, ZMQ_DONTWAIT | ZMQ_SNDMORE);
          this->proxy.send(static_cast<void *>(&buffer[pos + 1]), length - 1, ZMQ_DONTWAIT);
          ++forwarded;
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
        this->proxy.send(requester, ZMQ_DONTWAIT | ZMQ_SNDMORE);
        this->proxy.send(static_cast<const void*>(piece), length, ZMQ_DONTWAIT);
        ++forwarded;
        delim = next_delim;
      }
      buffer.assign(delim, end - delim);
      return forwarded;
    }
  };

  struct netstring_request_t {
    static void format(std::string& message) {
      message = std::to_string(message.size()) + ':' + message + ',';
    }
  };

  struct netstring_response_t {
    static void format(std::string& message) {
      message = std::to_string(message.size()) + ':' + message + ',';
    }
  };

}

#endif //__NETSTRING_PROTOCOL_HPP__
