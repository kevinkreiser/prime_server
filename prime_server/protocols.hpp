#ifndef __PROTOCOLS_HPP__
#define __PROTOCOLS_HPP__

#include <zmq.h>
#include <unordered_map>

namespace prime_server {

  //TODO: this kind of sucks but currently dont have a better way
  //might want to make it a real class and keep state as to where you are in parsing
  //this is like netstrings but the strings are actually binary if you want them to be
  class netstring_protocol_t {
   public:
    static zmq::message_t delineate(const void* data, size_t size) {
      auto size_prefix = std::to_string(size) + ':';
      zmq::message_t message(size_prefix.size() + size + 1);
      auto* dst = static_cast<char*>(message.data());
      std::copy(size_prefix.begin(), size_prefix.end(), dst);
      dst += size_prefix.size();
      std::copy(static_cast<const char*>(data), static_cast<const char*>(data) + size, dst);
      dst[size] = ',';
      return message;
    }
    static std::list<std::pair<const void*, size_t> > separate(const void* data, size_t size, size_t& consumed) {
      if(size == 0)
        return {};

      if(static_cast<const char*>(data)[0] == ':')
        throw std::runtime_error("Netstring protocol message cannot begin with a ':'");

      //keep getting pieces if there is enough space for the message length plus a message
      std::list<std::pair<const void*, size_t> > pieces;
      const char* begin = static_cast<const char*>(data);
      const char* end = begin + size;
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
        pieces.emplace_back(static_cast<const void*>(piece), length);
        delim = next_delim;
      }
      consumed = delim - static_cast<const char*>(data);
      return pieces;
    }
   protected:
  };


  //TODO: this only supports GET right now and is basically bare minimum, total work in progress..
  //TODO: should just re-think the whole protocol thing to be more of an object that keeps state for
  //assembling a request at the server or response at the client, creating a request or response needs
  //no state but would still possibly benefit from it (ie adding headers). see giant commented out stuff
  //below
  class http_protocol_t {
    //requests look like this
    //GET /primes?possible_prime=147 HTTP/1.1\r\nUser-Agent: fake\r\nHost: ipc\r\nAccept: */*\r\n\r\n
    //GET /help?blah=4 HTTP/1.1\r\nHost: localhost:8002\r\nUser-Agent: Mozilla/5.0 ... \r\n\r\n

   public:
    static zmq::message_t delineate(const void* data, size_t size) {
      const std::string directive("GET ");
      const std::string type_headers_etc(" HTTP/1.1\r\n\r\n");
      zmq::message_t message(directive.size() + size + type_headers_etc.size());
      auto* dst = static_cast<char*>(message.data());
      std::copy(directive.begin(), directive.end(), dst);
      dst += directive.size();
      std::copy(static_cast<const char*>(data), static_cast<const char*>(data) + size, dst);
      dst += size;
      std::copy(type_headers_etc.begin(), type_headers_etc.end(), dst);
      return message;
    }
    static std::list<std::pair<const void*, size_t> > separate(const void* data, size_t size, size_t& consumed) {
      if(size == 0)
        return {};

      if(static_cast<const char*>(data)[0] == '\r')
        throw std::runtime_error("Http protocol message cannot begin with a '\r'");

      //keep getting pieces if there is more
      std::list<std::pair<const void*, size_t> > pieces;
      const char* begin = static_cast<const char*>(data);
      const char* end = begin + size;
      const char* current = begin + 1;
      while(current < end) {
        if(current + 3 < end) {
          if(current[0] == '\r' && current[1] == '\n' && current[2] == '\r' && current[3] == '\n') {
            pieces.emplace_back(begin, (current + 4) - begin);
            begin = current + 4;
            current = begin + 1;
            continue;
          }
        }
        ++current;
      }
      consumed = begin - static_cast<const char*>(data);
      return pieces;
    }
/*
    enum class method_t { GET }; //, POST, PUT, HEAD, DELETE, TRACE, CONNECT };
    static const std::unordered_map<std::string, method_t> METHODS{ {"GET", method_t::GET} }; //, {"POST", method_t::POST}, {"PUT", method_t::PUT}, {"HEAD", method_t::HEAD}, {"DELETE", method_t::DELETE}, {"TRACE", method_t::TRACE}, {"CONNECT", method_t::CONNECT} };

    http_protocol_t() = delete;
    http_protocol_t(const char* str, size_t len): request(str, len) {
      auto next = parse_method(request.begin(), request.end());
      next = parse_path(next, request.end());
      next = parse_version(next, request.end());
      if(method == request.end() || path == request.end() || version == request.end())
        throw std::runtime_error("Invalid resource request");
      //TODO:
      //next = parse_headers(next, request.end());
      //if(method == method_t::POST)
      //  next = parse_post(next, request.end());
    }

    //the raw request line
    std::string request;
    //the method GET POST HEAD...
    method_t method;
    //path /what/you/want
    const char* path;
    //query items
    std::unordered_map<std::string, std::list<const char*> > query;
    //version of http
    const char* version;
    //TODO: std::unordered_map<std::string, std::list<const char *> > headers;
    //TODO: post data

   private:
    std::string::iterator parse_method(std::string::iterator begin, std::string::iterator end) {
      char* method_str = begin;
      //go through the range
      while(begin != end) {
        if(*begin == ' ') {
          *begin = '\0';
          ++begin;
          break;
        }
        ++begin;
      }
      auto method_itr = METHODS.find(method_str);
      if(method_itr == METHODS.end())
        throw std::runtime_error("Invalid http method");
      method = method_itr->second;
      return begin;
    }
    std::string::iterator parse_path(std::string::iterator begin, std::string::iterator end) {
      path = begin;
      bool has_query = false;
      //go through the range
      while(begin != end) {
        if(*begin == ' ') {
          *begin = '\0';
          ++begin;
          break;
        }
        else if(*begin == '?') {
          *begin = '\0';
          has_query = true;
          ++begin;
          break;
        }
        ++begin;
      }
      //check for query bits
      if(has_query) {
        return parse_query(begin, end);
      }
      return begin;
    }
    std::string::iterator parse_query(std::string::iterator begin, std::string::iterator end) {
      char* key = begin;
      char* value = nullptr;
      //go through the range
      while(begin != end) {
        switch(*begin) {
          case ' ':
            begin = '\0';
            return ++begin;
            break;
          case '&':
            begin = '\0';
            if(key != begin) {
              //update
              auto kv_itr = query.find(key);
              if(kv_itr != query.end())
                kv_itr->second.push_back(value);
              //new one
              else
                query.insert({key, {value}});
            }
            key = ++begin;
            value = nullptr;
            break;
          case '=':
            begin = '\0';
            value = ++begin;
            break;
          default:
            ++begin;
            break;
        }
      }
    }
    std::string::iterator parse_version(std::string::iterator begin, std::string::iterator end) {
      version = begin;
      //go through the range
      while(begin != end) {
        if(*begin == '\r' && begin + 1 != end && *(begin + 1) == '\n') {
          *begin = '\0';
          begin += 2;
          break;
        }
        ++begin;
      }
      return begin;
    }

*/
  };

}

#endif //__PROTOCOLS_HPP__
