#ifndef __HTTP_PROTOCOL_HPP__
#define __HTTP_PROTOCOL_HPP__

#include "prime_server.hpp"

#include <limits>
#include <cctype>

namespace {

  //check if base starts with pattern
  bool startswith(const char* base, const char *pattern) {
    if(base == nullptr || pattern == nullptr)
      return false;
    while(*base != '\0' && *pattern != '\0') {
      if(*base != *pattern)
        return false;
      ++base;
      ++pattern;
    }
    return true;
  }

  const std::string CONTENT_LENGTH("\r\nContent-Length: ");
  const std::string DOUBLE_RETURN("\r\n\r\n");
}

//http1.1 is a complete mess of a protocol.. lets not implement it for now..
//http://www.w3.org/Protocols/rfc2616/rfc2616-sec4.html#sec4.4

namespace prime_server {

  class http_client_t : public client_t {
   public:
    http_client_t(zmq::context_t& context, const std::string& server_endpoint, const request_function_t& request_function,
      const collect_function_t& collect_function, size_t batch_size = 8912):
      client_t(context, server_endpoint, request_function, collect_function, batch_size){
      reset();
    }
   protected:
    virtual size_t stream_responses(const void* message, size_t size, bool& more) {
      //um..
      size_t collected = 0;
      if(size == 0)
        return collected;

      //box out the message a bit so we can iterate
      const char* begin = static_cast<const char*>(message);
      const char* end = begin + size;

      //sample responses:
      //HTTP/1.0 200 OK\r\nContent-Length: 4\r\nSomeHeader: blah\r\n\r\nbody\r\n\r\n
      //HTTP/1.0 304 Not Modified\r\nSomeHeader: blah\r\n\r\n

      //this is a very forgiving state machine (ignores content-length's actual value)
      const char* current = begin;
      for(; current < end; ++current) {
        //if we are trying to get the body
        if(parse_body) {
          //still eating
          if(length--)
            continue;
          //done look for the end
          else {
            parse_body = false;
            parse_ending = true;
          }
        }

        //check for the numbers in the length part
        if(parse_length) {
          //done parsing length header
          if(!isdigit(*current)) {
            length = std::stoul(content_length);
            parse_length = false;
          }//keep this char of the length
          else {
            content_length.push_back(*current);
            continue;
          }
        }

        //check for content length header
        if(parse_headers) {
          if(content_length_itr != CONTENT_LENGTH.cend() && *current == *content_length_itr) {
            ++content_length_itr;
            if(content_length_itr == CONTENT_LENGTH.cend()) {
              parse_headers = false;
              parse_length = true;
              parse_ending = false;
            }
          }
          else
            content_length_itr =  CONTENT_LENGTH.cbegin();
        }

        //check for double return
        if(*current == *double_return_itr) {
          ++double_return_itr;
          //found the double return
          if(double_return_itr == DOUBLE_RETURN.cend()) {
            //we just found an ending but because there was a length header we aren't done
            if(!parse_ending) {
              parse_body = true;
              double_return_itr = DOUBLE_RETURN.cbegin();
              continue;
            }//ready to send from buffer
            else if(buffer.size()) {
              buffer.append(begin, (current - begin) + 1);
              more = collect_function(static_cast<const void*>(buffer.data()), buffer.size());
              buffer.clear();
              ++collected;
              begin = current + 1;
            }//ready to send from stream
            else {
              more = collect_function(static_cast<const void*>(begin), (current - begin) + 1);
              ++collected;
              begin = current + 1;
            }
            reset();
          }
        }
        else
          double_return_itr = DOUBLE_RETURN.cbegin();
      }

      buffer.append(begin, current - begin);
      return collected;
    }
    void reset() {
      parse_headers = true;
      parse_length = false;
      parse_body = false;
      parse_ending = true;
      content_length.clear();
      length = 0;
      double_return_itr = DOUBLE_RETURN.cbegin();
      content_length_itr = CONTENT_LENGTH.cbegin();
    }

    bool parse_headers;
    bool parse_length;
    bool parse_body;
    bool parse_ending;
    std::string content_length;
    size_t length;

    std::string::const_iterator double_return_itr;
    std::string::const_iterator content_length_itr;

    std::string buffer;
  };

  //TODO: dont use a string, make another object to keep the machines state
  class http_server_t : public server_t<std::string> {
   public:
    using server_t<std::string>::server_t;
    virtual ~http_server_t(){}
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
        while(begin < end) {
          buffer.push_back(*begin);
          ++begin;
          if(buffer.size() > 4 && buffer.find("\r\n\r\n", buffer.size() - 4) != std::string::npos) {
            this->proxy.send(requester, ZMQ_DONTWAIT | ZMQ_SNDMORE);
            this->proxy.send(buffer, ZMQ_DONTWAIT);
            ++forwarded;
            break;
          }
        }
      }

      //keep getting pieces while they are there
      if(begin < end && *begin != 'G')
        throw std::runtime_error("Http protocol request cannot begin with anything other than 'GET'");
      const char* current = begin;
      while(current < end) {
        ++current;
        if(current - begin > 4 && startswith(current - 4, "\r\n\r\n")) {
          this->proxy.send(requester, ZMQ_DONTWAIT | ZMQ_SNDMORE);
          this->proxy.send(static_cast<const void*>(begin), current - begin, ZMQ_DONTWAIT);
          ++forwarded;
          begin = current;
        }
      }
      buffer.assign(begin, end - begin);
      return forwarded;
    }
  };

  struct http_request_t {
    static std::string get(const std::string& uri/*, add headers*/) {
      return "GET " + uri + " HTTP/1.0\r\n\r\n";
    }
  };

  //TODO: support gzip encoded responses natively or let applications to do this themselves?

  struct http_response_t {
    static std::string ok(const std::string&  body/*,add headers*/) {
      return "HTTP/1.0 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body + "\r\n\r\n";
    }
  };

  //requests look like this
  //GET /primes?possible_prime=147 HTTP/1.1\r\nUser-Agent: fake\r\nHost: ipc\r\nAccept: */*\r\n\r\n
  //GET /help?blah=4 HTTP/1.1\r\nHost: localhost:8002\r\nUser-Agent: Mozilla/5.0 ... \r\n\r\n

/*
  class http_request_t {
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
  };
*/

}

#endif //__HTTP_PROTOCOL_HPP__
