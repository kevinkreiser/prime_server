#ifndef __NETSTRING_PROTOCOL_HPP__
#define __NETSTRING_PROTOCOL_HPP__

#include "prime_server.hpp"

#include <limits>

namespace prime_server {

  class netstring_client_t : public client_t {
   public:
    using client_t::client_t;
   protected:
    virtual size_t stream_responses(void* message, size_t size, bool& more) {
      //um..
      size_t collected = 0;
      if(size == 0)
        return collected;

      //box out the message a bit so we can iterate
      const char* begin = static_cast<const char*>(message);
      const char* end = begin + size;
      const char* delim = begin;

      //do we have a partial response
      if(buffer.size()) {
        //make sure the buffer has a colon in it and bail otherwise
        size_t length = std::numeric_limits<size_t>::max();
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
          size = end - begin;
        }

        //copy up to what the message calls for
        length = std::stoul(buffer.substr(0, pos));
        length -= buffer.size() - (pos + 1);

        //not enough
        if(length > size) {
          buffer.append(begin, end - begin);
          return collected;
        }//we have a full message here
        else {
          buffer.append(begin, length);
          more = collect_function(static_cast<const void*>(buffer.data()), buffer.size());
          ++collected;
          begin += length;
          delim += length;
        }
      }

      //keep getting pieces if there is enough space for the message length plus a message
      if(static_cast<const char*>(message)[0] == ':')
        throw std::runtime_error("Netstring protocol message cannot begin with a ':'");
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

  class netstring_server_t : public server_t {
   public:
    using server_t::server_t;
   protected:
    virtual void stream_requests(void* message, size_t size, const std::string& requester, std::string& buffer) {
      //um..
      if(size == 0)
        return;

      //box out the message a bit so we can iterate
      const char* begin = static_cast<const char*>(message);
      const char* end = begin + size;
      const char* delim = begin;

      //do we have a partial request
      if(buffer.size()) {
        //make sure the buffer has a colon in it and bail otherwise
        size_t length = std::numeric_limits<size_t>::max();
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
          size = end - begin;
        }

        //copy up to what the message calls for
        length = std::stoul(buffer.substr(0, pos));
        length -= buffer.size() - (pos + 1);

        //not enough
        if(length > size) {
          buffer.append(begin, end - begin);
          return;
        }//we have a full message here
        else {
          buffer.append(begin, length);
          proxy.send(requester, ZMQ_SNDMORE);
          proxy.send(buffer, 0);
          begin += length;
          delim += length;
        }
      }

      //keep getting pieces if there is enough space for the message length plus a message
      if(static_cast<const char*>(message)[0] == ':')
        throw std::runtime_error("Netstring protocol message cannot begin with a ':'");
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
        proxy.send(requester, ZMQ_SNDMORE);
        proxy.send(static_cast<const void*>(piece), length, 0);
        delim = next_delim;
      }
      buffer.assign(delim, end - delim);
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
