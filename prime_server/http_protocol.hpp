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


  using headers_t = std::unordered_map<std::string, std::string>;
  using query_t = std::unordered_map<std::string, std::list<std::string> >;
  enum method_t { GET, POST, PUT, HEAD, DELETE, TRACE, CONNECT };
  const std::unordered_map<std::string, method_t> STRING_TO_METHOD {
    {"GET", method_t::GET}, {"POST", method_t::POST}, {"PUT", method_t::PUT}, {"HEAD", method_t::HEAD},
    {"DELETE", method_t::DELETE}, {"TRACE", method_t::TRACE}, {"CONNECT", method_t::CONNECT}
  };
  const std::unordered_map<method_t, std::string, std::hash<int> > METHOD_TO_STRING{
    {method_t::GET, "GET"}, {method_t::POST, "POST"}, {method_t::PUT, "PUT"}, {method_t::HEAD, "HEAD"},
    {method_t::DELETE, "DELETE"}, {method_t::TRACE, "TRACE"}, {method_t::CONNECT, "CONNECT"}
  };
  struct http_request_t {
   public:
    method_t method;
    std::string path;
    query_t query;
    std::string version;
    headers_t headers;
    std::string body;

    http_request_t() {
      path = version = body = "";
      cursor = end = nullptr;
      flush_stream();
    }

    http_request_t(const method_t& method, const std::string& path, const std::string& body = "", const query_t& query = query_t{},
                   const headers_t& headers = headers_t{}, const std::string& version = "HTTP/1.1") :
                   method(method), path(path), body(body), query(query), headers(headers), version(version) {
      state = METHOD;
      delimeter = " ";
      partial_length = 0;
      body_length = 0;
      consumed = 0;
      partial_buffer.clear();
      cursor = end = nullptr;
    }

    std::string to_string() const {
      return to_string(method, path, body, query, headers, version);
    }

    static std::string to_string(const method_t& method, const std::string& path, const std::string& body = "", const query_t& query = query_t{},
                                 const headers_t& headers = headers_t{}, const std::string& version = "HTTP/1.1") {
      auto itr = METHOD_TO_STRING.find(method);
      if(itr == METHOD_TO_STRING.end())
        throw std::runtime_error("Unsupported http request method");
      std::string request = itr->second + ' ';
      request += path;

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
      if((body.size() || method == method_t::POST) && headers.find("Content-Length") == headers.end()) {
        request += "Content-Length: ";
        request += std::to_string(body.size());
        request += "\r\n\r\n";
        request += body;
        request += "\r\n";
      }
      request += "\r\n";
      return request;
    }

    static http_request_t from_string(const char* start, size_t length) {
      http_request_t request;
      auto requests = request.from_stream(start, length);
      if(requests.size() == 0)
        throw std::runtime_error("Incomplete http request");
      return std::move(requests.front());
    }

    std::list<http_request_t> from_stream(const char* start, size_t length) {
      std::list<http_request_t> requests;
      cursor = start;
      end = start + length;
      while(start != end) {
        //grab up to the next return
        if(!consume_until())
          break;

        //what are we looking to parse
        switch(state) {
          case METHOD: {
            auto itr = STRING_TO_METHOD.find(partial_buffer);
            if(itr == STRING_TO_METHOD.end())
              throw std::runtime_error("Unknown http method");
            method = itr->second;
            state = PATH;
            break;
          }
          case PATH: {
            path.swap(partial_buffer);
            delimeter = "\r\n";
            state = VERSION;
            break;
          }
          case VERSION: {
            version.swap(partial_buffer);
            if(version != "HTTP/1.0" && version != "HTTP/1.1")
              throw std::runtime_error("Unknown http version");
            //split off the query bits
            auto pos = path.find('?');
            size_t key_pos = pos, value_pos;
            while(key_pos++ != std::string::npos && (value_pos = path.find('=', key_pos)) != std::string::npos) {
              auto key = path.substr(key_pos, value_pos++ - key_pos);
              key_pos = path.find('&', value_pos);
              auto value = path.substr(value_pos, key_pos - value_pos);

              auto kv = query.find(key);
              if(kv == query.cend())
                query.insert({key, {value}});
              else
                kv->second.emplace_back(std::move(value));
            }
            if(pos != std::string::npos)
              path.resize(pos);
            state = HEADERS;
            break;
          }
          case HEADERS: {
            //a header is here
            if(partial_buffer.size()) {
              auto pos = partial_buffer.find(": ");
              if(pos == std::string::npos)
                throw std::runtime_error("Expected http header");
              headers.insert({partial_buffer.substr(0, pos), partial_buffer.substr(pos + 2)});
            }//the end or body
            else {
              auto length_str = headers.find("Content-Length");
              if(length_str != headers.end()) {
                body_length = std::stoul(length_str->second);
                state = BODY;
              }
              else {
                //TODO: check for chunked
                requests.push_back(http_request_t(method, path, body, query, headers, version));
                flush_stream();
              }
            }
            break;
          }
          case BODY: {
            //TODO: actually use body length
            body.swap(partial_buffer);
            state = END;
            break;
          }
          case CHUNKS: {
            //TODO: actually parse out the length and chunk by alternating
            //TODO: return 501
            throw std::runtime_error("not implemented");
          }
          case END: {
            if(partial_buffer.size())
              throw std::runtime_error("Unexpected data near end of http request");
            requests.push_back(http_request_t(method, path, body, query, headers, version));
            flush_stream();
            break;
          }
        }

        //next piece
        partial_buffer.clear();
      }

      return requests;
    }
    void flush_stream() {
      state = METHOD;
      delimeter = " ";
      partial_length = 0;
      body_length = 0;
      consumed = 0;
      partial_buffer.clear();
      path.clear();
      query.clear();
      version.clear();
      headers.clear();
      body.clear();
    }
    size_t size() const {
      return consumed + partial_buffer.size();
    }


   protected:
    //state for streaming parsing
    const char *cursor, *end, *delimeter;
    std::string partial_buffer;
    size_t partial_length;
    enum state_t { METHOD, PATH, VERSION, HEADERS, BODY, CHUNKS, END };
    state_t state;
    size_t body_length;
    size_t consumed;

    bool consume_until() {
      //go until we run out or we found the delimeter
      const char* current = cursor;
      char c;
      while((c = *(delimeter + partial_length)) != '\0' && current != end) {
        if(c != *current)
          partial_length = 0;
        else
          ++partial_length;
        ++current;
      }
      //we found the delimeter
      if(c == '\0') {
        size_t length = current - cursor;
        if(length < partial_length)
          partial_buffer.resize(partial_buffer.size() - (partial_length - length));
        else
          partial_buffer.append(cursor, current - partial_length);
        partial_length = 0;
        cursor = current;
        consumed += length;
        return true;
      }
      //we ran out
      partial_buffer.assign(cursor, current - cursor);
      return false;
    }

  };

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

  class http_server_t : public server_t<http_request_t> {
     public:
      using server_t<http_request_t>::server_t;
      virtual ~http_server_t(){}
     protected:
      virtual size_t stream_requests(const void* message, size_t size, const std::string& requester, http_request_t& request) {
        auto requests = request.from_stream(static_cast<const char*>(message), size);
        for(const auto& parsed_request : requests) {
          this->proxy.send(requester, ZMQ_DONTWAIT | ZMQ_SNDMORE);
          this->proxy.send(parsed_request.to_string(), ZMQ_DONTWAIT);
        }
        return requests.size();
      }
    };

}

#endif //__HTTP_PROTOCOL_HPP__
