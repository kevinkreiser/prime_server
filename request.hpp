#ifndef __HTTP_REQUEST_HPP__
#define __HTTP_REQUEST_HPP__

#include <unordered_map>

namespace http {
  //TODO: this only supports GET right now
  //TODO: write tests
  class request_t {
   public:
    enum class method_t { GET/*, POST, PUT, HEAD, DELETE, TRACE, CONNECT*/ };
    static const std::unordered_map<std::string, method_t> METHODS = { {"GET", method_t::GET}/*, {"POST", method_t::POST}, {"PUT", method_t::PUT}, {"HEAD", method_t::HEAD}, {"DELETE", method_t::DELETE}, {"TRACE", method_t::TRACE}, {"CONNECT", method_t::CONNECT}*/ };

    request_t() = delete;
    //requests look like this
    //GET /help?blah=4 HTTP/1.1\r\nHost: localhost:8002\r\nUser-Agent: Mozilla/5.0 ... \r\n\r\n
    request_t(const char* str, size_t len): request(str, len) {
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
  };
}

#endif //__HTTP_REQUEST_HPP__
