#ifndef __HTTP_PROTOCOL_HPP__
#define __HTTP_PROTOCOL_HPP__

#include <prime_server/prime_server.hpp>

#include <limits>
#include <cctype>
#include <unordered_map>
#include <list>

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

  std::string consume_until(const std::string& delimeter, std::string::const_iterator& start, const std::string::const_iterator& end) {
    std::string::const_iterator current = start;
    auto i = delimeter.cbegin();
    while(i != delimeter.cend() && current != end) {
      if(*i != *current)
        i = delimeter.cbegin();
      if(*i == *current)
        ++i;
      ++current;
    }
    auto part = std::string(start, current - (i == delimeter.cend() ? delimeter.size() : 0));
    start = current;
    return part;
  };

  //TODO: implement a ring buffer and pass into it a bunch of fixed string that you want to detect
  //then you can ask the buffer on each iteration whether any of the strings are currently met
  //you can also remove match strings from the buffer. the buffer only grows in size if you add
  //a string larger than the buffer's size, can probably use the keep on open strategy. anyway
  //the buffer can be used as a state in the fsm
  const std::string CONTENT_LENGTH("\r\nContent-Length: ");
  const std::string DOUBLE_RETURN("\r\n\r\n");
}

//http1.1 is a complete mess of a protocol.. lets not implement it for now..
//http://www.w3.org/Protocols/rfc2616/rfc2616-sec4.html#sec4.4

namespace prime_server {

  //TODO:
  std::string url_encode(const std::string& unencoded) {
    throw std::runtime_error("unimplemented");
  }

  //TODO:
  std::string url_decode(const std::string& unencoded) {
    throw std::runtime_error("unimplemented");
  }

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


  using headers_t = std::unordered_map<std::string, std::string>;
  using query_t = std::unordered_map<std::string, std::list<std::string> >;
  enum class method_t : size_t { GET, POST, PUT, HEAD, DELETE, TRACE, CONNECT };
  const std::unordered_map<std::string, method_t> STRING_TO_METHOD{ {"GET", method_t::GET}, {"POST", method_t::POST}, {"PUT", method_t::PUT}, {"HEAD", method_t::HEAD}, {"DELETE", method_t::DELETE}, {"TRACE", method_t::TRACE}, {"CONNECT", method_t::CONNECT} };
  struct http_request_t {
    method_t method;
    std::string path;
    query_t query;
    std::string version;
    headers_t headers;
    std::string body;

    std::string get() { return get(path, headers); };
    std::string post() { return post(path, body, headers); };
    std::string head() { return head(path, headers); }
    std::string delete_() { return delete_(path, headers); }
    std::string trace() { return trace(path, headers); }
    std::string connect() { return connect(path, headers); }

    static http_request_t parse(const std::string& bytes) {
      http_request_t request;

      //nice way to eat bytes up to a certain point
      std::string::const_iterator start = bytes.cbegin();
      auto consume_header = [&start, &bytes, &request]() {
        auto part = consume_until("\r\n", start, bytes.cend());
        //hit the end of request or begin of body
        if(part.size() == 0)
         return false;
        auto pos = part.find(": ");
        if(pos == std::string::npos)
          throw std::runtime_error("Expected http header");
        request.headers.insert({part.substr(0, pos), part.substr(pos + 2)});
        return true;
      };

      //method
      auto method_str = consume_until(" ", start, bytes.cend());
      auto method = STRING_TO_METHOD.find(method_str);
      if(method == STRING_TO_METHOD.end())
        throw std::runtime_error("Unknown http method");
      request.method = method->second;
      //path
      request.path = consume_until(" ", start, bytes.cend());
      //version
      request.version = consume_until("\r\n", start, bytes.cend());
      if(request.version != "HTTP/1.0" && request.version != "HTTP/1.1")
        throw std::runtime_error("Unknown http version");

      //split off the query
      auto pos = request.path.find('?');
      if(pos != std::string::npos) {
        auto query = request.path.substr(pos + 1);
        request.path.resize(pos);
        std::string key, value;
        std::string::const_iterator start = query.cbegin();
        while((key = consume_until("=", start, query.cend())).size() && (value = consume_until("&", start, query.cend())).size()) {
          auto kv = request.query.find(key);
          if(kv == request.query.cend())
            request.query.insert({key, {value}});
          else
            kv->second.push_back(value);
        }
      }

      //headers
      while(consume_header());

      //body
      auto length_str = request.headers.find("Content-Length");
      if(length_str == request.headers.end())
        return request;
      auto length = std::stoul(length_str->second);
      request.body = bytes.substr(start - bytes.cbegin());

      return request;
    }
    static std::string get(const std::string& path, const headers_t& headers,
                           const query_t& query = query_t{}, const std::string& version = "HTTP/1.0") {
      std::string request = "GET " + path;

      //query string
      if(query.size()) {
        request.push_back('?');
        bool amp = false;
        for(const auto& kv : query) {
          //TODO: support blank parameters?
          for(const auto& v : kv.second) {
            if(amp)
              request.push_back('&');
            amp = true;
            //TODO: url encode
            request += kv.first;
            request.push_back('=');
            request += v;
          }
        }
      }

      //version
      request.push_back(' ');
      request += version;
      request += "\r\n";

      //headers
      for(const auto& header : headers) {
        request += header.first;
        request += ": ";
        request += header.second;
        request += "\r\n";
      }

      //done
      request += "\r\n";
      return request;
    }
    static std::string post(const std::string& path, const std::string& body, const headers_t& headers,
                            const query_t& query = query_t{}, const std::string& version = "HTTP/1.0") {
      std::string request = "POST " + path;

      //query string
      if(query.size()) {
        request.push_back('?');
        bool amp = false;
        for(const auto& kv : query) {
          //TODO: support blank parameters?
          for(const auto& v : kv.second) {
            if(amp)
              request.push_back('&');
            amp = true;
            //TODO: url encode
            request += kv.first;
            request.push_back('=');
            request += v;
          }
        }
      }

      //version
      request.push_back(' ');
      request += version;
      request += "\r\n";

      //headers
      for(const auto& header : headers) {
        request += header.first;
        request += ": ";
        request += header.second;
        request += "\r\n";
      }

      //body
      request += "Content-Length: ";
      request += std::to_string(body.size());
      request += "\r\n\r\n";
      request += body;
      request += "\r\n\r\n";
      return request;
    }
    static std::string head(const std::string& path, const headers_t& headers) {
      throw std::runtime_error("unimplemented");
    }
    static std::string delete_(const std::string& path, const headers_t& headers) {
      throw std::runtime_error("unimplemented");
    }
    static std::string trace(const std::string& path, const headers_t& headers) {
      throw std::runtime_error("unimplemented");
    }
    static std::string connect(const std::string& path, const headers_t& headers) {
      throw std::runtime_error("unimplemented");
    }
  };

  //TODO: support gzip encoded responses natively or let applications to do this themselves?


  struct http_response_t {
    static std::string generic(unsigned code, const std::string message, const headers_t& headers, const std::string& body = "") {
      auto response = "HTTP/1.0 " + std::to_string(code) + ' ' + message + "\r\n";
      for(const auto& header : headers) {
        response += header.first;
        response += ": ";
        response += header.second;
        response += "\r\n";
      }
      if(body.size()){
        response += "Content-Length: ";
        response += std::to_string(body.size());
        response += "\r\n\r\n";
        response += body + "\r\n";
      }
      response.append("\r\n");
      return response;
    }
  };

}

#endif //__HTTP_PROTOCOL_HPP__
