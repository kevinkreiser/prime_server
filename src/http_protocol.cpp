#include "http_protocol.hpp"
#include "logging.hpp"

#include <curl/curl.h>

//TODO: someone please replace the http protocol, its a giant mess. kthxbye

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

  std::string url_encode(const std::string& unencoded) {
    char* encoded = curl_escape(unencoded.c_str(), static_cast<int>(unencoded.size()));
    if(encoded == nullptr)
      throw std::runtime_error("url encoding failed");
    std::string encoded_str(encoded);
    curl_free(encoded);
    return encoded_str;
  }

  std::string url_decode(const std::string& encoded) {
    char* decoded = curl_unescape(encoded.c_str(), static_cast<int>(encoded.size()));
    if(decoded == nullptr)
      throw std::runtime_error("url decoding failed");
    std::string decoded_str(decoded);
    curl_free(decoded);
    return decoded_str;
  }
}

namespace prime_server {

  http_client_t::http_client_t(zmq::context_t& context, const std::string& server_endpoint, const request_function_t& request_function,
    const collect_function_t& collect_function, size_t batch_size):
    client_t(context, server_endpoint, request_function, collect_function, batch_size){
    reset();
  }
  size_t http_client_t::stream_responses(const void* message, size_t size, bool& more) {
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
  void http_client_t::reset() {
    parse_headers = true;
    parse_length = false;
    parse_body = false;
    parse_ending = true;
    content_length.clear();
    length = 0;
    double_return_itr = DOUBLE_RETURN.cbegin();
    content_length_itr = CONTENT_LENGTH.cbegin();
  }

  http_entity_t::http_entity_t(const std::string& version, const headers_t& headers, const std::string& body):
    version(version), headers(headers), body(body) {
  }

  http_entity_t::~http_entity_t(){}


  http_request_t::http_request_t():http_entity_t("", headers_t{}, ""){
    path = version = body = "";
    cursor = end = nullptr;
    flush_stream();
  }

  http_request_t::http_request_t(const method_t& method, const std::string& path, const std::string& body, const query_t& query,
                 const headers_t& headers, const std::string& version) :
                 http_entity_t(version, headers, body), method(method), path(path), query(query) {
    state = METHOD;
    delimeter = " ";
    partial_length = 0;
    body_length = 0;
    consumed = 0;
    partial_buffer.clear();
    cursor = end = nullptr;
  }

 http_request_t::info_t http_request_t::to_info(uint64_t id) const {
    auto header = headers.find("Connection");
    return info_t {
      id,
      static_cast<uint64_t>(version == "HTTP/1.0" ? 0 : 1),
      static_cast<uint64_t>(header != headers.end() && header->second == "Keep-Alive"),
      static_cast<uint64_t>(header != headers.end() && header->second == "Close")
    };
  }

  std::string http_request_t::to_string() const {
    return to_string(method, path, body, query, headers, version);
  }

  std::string http_request_t::to_log(const method_t& method, const std::string& path, const query_t& query, const std::string& version) {
    auto itr = METHOD_TO_STRING.find(method);
    if(itr == METHOD_TO_STRING.end())
      throw std::runtime_error("Unsupported http request method");
    std::string request = itr->second + ' ';


    //path and query string
    std::string pq = path;
    if(query.size()) {
      pq.push_back('?');
      bool amp = false;
      for(const auto& kv : query) {
        //TODO: support blank parameters?
        for(const auto& v : kv.second) {
          if(amp)
            pq.push_back('&');
          amp = true;
          pq += kv.first;
          pq.push_back('=');
          pq += v;
        }
      }
    }
    request += url_encode(pq);

    //version
    request.push_back(' ');
    request += version;
    return request;
  }

  std::string http_request_t::to_string(const method_t& method, const std::string& path, const std::string& body, const query_t& query,
                               const headers_t& headers, const std::string& version) {
    auto request = to_log(method, path, query, version);
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

  http_request_t http_request_t::from_string(const char* start, size_t length) {
    http_request_t request;
    auto requests = request.from_stream(start, length);
    if(requests.size() == 0)
      throw std::runtime_error("Incomplete http request");
    return std::move(requests.front());
  }

  std::list<http_request_t> http_request_t::from_stream(const char* start, size_t length) {
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
          path = url_decode(partial_buffer);
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
  void http_request_t::flush_stream() {
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
  size_t http_request_t::size() const {
    return consumed + partial_buffer.size();
  }

  bool http_request_t::consume_until() {
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


  http_response_t::http_response_t(unsigned code, const std::string& message, const std::string& body, const headers_t& headers,
                  const std::string& version) :
                  http_entity_t(version, headers, body), code(code), message(message) {
  }

  void http_response_t::from_info(const http_request_t::info_t* info) {
      version = info->version ? "HTTP/1.1" : "HTTP/1.0";
      if(info->connection_keep_alive)
        headers.emplace("Connection", "Keep-Alive");
      if(info->connection_close)
        headers.emplace("Connection", "Close");
  }

  std::string http_response_t::to_string() const {
    return generic(code, message, headers, body, version);
  }

  std::string http_response_t::generic(unsigned code, const std::string message, const headers_t& headers, const std::string& body, const std::string& version) {
    auto response = version;
    response.push_back(' ');
    response += std::to_string(code);
    response.push_back(' ');
    response += message;
    response += "\r\n";

    for(const auto& header : headers) {
      if(header.first == "Content-Length")
        continue;
      response += header.first;
      response += ": ";
      response += header.second;
      response += "\r\n";
    }
    response += "Content-Length: ";
    response += std::to_string(body.size());
    response += "\r\n\r\n";
    if(body.size())
      response += body + "\r\n\r\n";
    return response;
  }

  http_server_t::http_server_t(zmq::context_t& context, const std::string& client_endpoint, const std::string& proxy_endpoint, const std::string& result_endpoint, bool log):
    server_t<http_request_t, http_request_t::info_t>::server_t(context, client_endpoint, proxy_endpoint, result_endpoint, log), request_id(0) {
  }
  http_server_t::~http_server_t(){}

  void http_server_t::enqueue(const void* message, size_t size, const std::string& requester, http_request_t& request) {
    auto requests = request.from_stream(static_cast<const char*>(message), size);
    for(const auto& parsed_request : requests) {
      //figure out if we are expecting to close this request or not
      auto info = parsed_request.to_info(request_id);
      //send on the request
      this->proxy.send(requester, ZMQ_DONTWAIT | ZMQ_SNDMORE);
      this->proxy.send(static_cast<const void*>(&info), sizeof(info), ZMQ_DONTWAIT | ZMQ_SNDMORE);
      this->proxy.send(parsed_request.to_string(), ZMQ_DONTWAIT);
      if(log) {
        std::string log_line = std::to_string(request_id);
        log_line.push_back(' ');
        LOG_INFO(log_line + http_request_t::to_log(parsed_request.method, parsed_request.path, parsed_request.query, parsed_request.version));
      }
      //remember we are working on it
      this->requests.emplace(request_id++, requester);
    }
  }
  void http_server_t::dequeue(const http_request_t::info_t& request_info) {
    auto request = requests.find(request_info.id);
    if(request != requests.end()) {
      //close the session
      if((request_info.version == 0 && !request_info.connection_keep_alive) ||
         (request_info.version == 1 && request_info.connection_close)){
        this->client.send(request->second, ZMQ_DONTWAIT | ZMQ_SNDMORE);
        this->client.send(static_cast<const void*>(""), 0, ZMQ_DONTWAIT);
        sessions.erase(request->second);
      }
      requests.erase(request);
      if(log)
        LOG_INFO(std::to_string(request_info.id) + " REPLIED");
    }
    else
      LOG_WARN("Unknown or timed-out request id: " + std::to_string(request_info.id));
  }
}
