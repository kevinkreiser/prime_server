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

  void log_request(uint64_t id, const std::string& request) {
    auto log_line = std::to_string(id);
    log_line.push_back(' ');
    log_line += logging::timestamp();
    log_line.push_back(' ');
    log_line += request;
    log_line.push_back('\n');
    logging::log(log_line);
  }

  void log_response(uint64_t id, uint16_t code, size_t length) {
    auto log_line = std::to_string(id);
    log_line.push_back(' ');
    log_line += logging::timestamp();
    log_line.push_back(' ');
    log_line += std::to_string(code);
    log_line.push_back(' ');
    log_line += std::to_string(length);
    log_line.push_back('\n');
    logging::log(log_line);
  }

  struct request_exception_t : public std::runtime_error {
    request_exception_t(const prime_server::http_response_t& response): runtime_error(response.to_string()), code(response.code), response(response.to_string()) { }
    const uint16_t code;
    const std::string response;
  };

  const prime_server::headers_t::value_type CORS{"Access-Control-Allow-Origin", "*"};
  const request_exception_t RESPONSE_400(prime_server::http_response_t(400, "Bad Request", "Malformed HTTP request", {CORS}));
  const request_exception_t RESPONSE_413(prime_server::http_response_t(413, "Request Entity Too Large", "The HTTP request was too large", {CORS}));
  const request_exception_t RESPONSE_501(prime_server::http_response_t(501, "Not Implemented", "The HTTP request method is not supported", {CORS}));
  const request_exception_t RESPONSE_505(prime_server::http_response_t(505, "HTTP Version Not Supported", "The HTTP request version is not supported", {CORS}));

  template <class T>
  size_t name_max(const std::unordered_map<std::string, T>& methods) {
    size_t i = 0;
    for(const auto& kv : methods)
      i = std::max(i, kv.first.size());
    return i;
  };
  const size_t METHOD_MAX_SIZE = name_max(prime_server::STRING_TO_METHOD) + 1;
  const size_t VERSION_MAX_SIZE = name_max(prime_server::SUPPORTED_VERSIONS) + 2;
}

namespace prime_server {

  http_client_t::http_client_t(zmq::context_t& context, const std::string& server_endpoint, const request_function_t& request_function,
    const collect_function_t& collect_function, size_t batch_size):
    client_t(context, server_endpoint, request_function, collect_function, batch_size){
    reset();
  }

  //TODO: make a container_t interface to this so that the client can hold the currently parsing response, then call stream_responses below
  size_t http_client_t::stream_responses(const void* message, size_t size, bool& more) {
    //um..
    size_t collected = 0;
    if(size == 0)
      return collected;

    //box out the message a bit so we can iterate
    const char* begin = static_cast<const char*>(message);
    const char* end = begin + size;
    bool done = false;

    //sample responses:
    //HTTP/1.0 200 OK\r\nContent-Length: 4\r\nSomeHeader: blah\r\n\r\nbody
    //HTTP/1.0 304 Not Modified\r\nSomeHeader: blah\r\n\r\n

    //this is a very forgiving state machine
    const char* current = begin;
    for(; current < end; ++current) {
      //if we are trying to get the body
      if(parse_body) {
        //eat until done
        if(--length == 0)
          done = true;
        else
          continue;
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
          }
        }
        else
          content_length_itr =  CONTENT_LENGTH.cbegin();
      }

      //check for double return separating headers from body or marking end
      if(*current == *double_return_itr) {
        ++double_return_itr;
        //found the double return
        if(double_return_itr == DOUBLE_RETURN.cend()) {
          //we just found an ending but because there was a length header we aren't done
          if(length)
            parse_body = true;
          else
            done = true;
        }
      }
      else
        double_return_itr = DOUBLE_RETURN.cbegin();

      //we have one ready
      if(done) {
        //ready to send from buffer
        if(buffer.size()) {
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
        done = false;
      }
    }

    buffer.append(begin, current - begin);
    return collected;
  }
  void http_client_t::reset() {
    parse_headers = true;
    parse_length = false;
    parse_body = false;
    content_length.clear();
    length = 0;
    double_return_itr = DOUBLE_RETURN.cbegin();
    content_length_itr = CONTENT_LENGTH.cbegin();
  }

  http_entity_t::http_entity_t(const std::string& version, const headers_t& headers, const std::string& body):
    version(version), headers(headers), body(body) {
    state = METHOD;
    delimiter = " ";
    partial_length = 0;
    body_length = 0;
    consumed = 0;
    cursor = end = nullptr;
  }

  http_entity_t::~http_entity_t(){}

  bool http_entity_t::consume_until() {
    const char* current = cursor;
    char c;
    //go until we consumed enough bytes
    if(body_length) {
      //while(current++ != end && --body_length);
      //we have enough
      if(end - current > body_length) {
        current += body_length;
        body_length = 0;
      }//we dont
      else {
        body_length -= end - current;
        current = end;
      }
      c = body_length != 0;
    }//go until delimiter
    else {
      while((c = *(delimiter + partial_length)) != '\0' && current != end) {
        if(c != *current)
          partial_length = 0;
        else
          ++partial_length;
        ++current;
      }
    }
    //we found the delimiter
    size_t length = current - cursor;
    if(c == '\0') {
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
    partial_buffer.append(cursor, length);
    return false;
  }

  void http_entity_t::flush_stream(const state_t current_state) {
    state = current_state;
    delimiter = " ";
    partial_length = 0;
    body_length = 0;
    consumed = 0;
    partial_buffer.clear();
    version.clear();
    headers.clear();
    body.clear();
  }

  http_request_t::http_request_t():http_entity_t("", headers_t{}, ""){
    path = "";
    flush_stream();
  }

  http_request_t::http_request_t(const method_t& method, const std::string& path, const std::string& body, const query_t& query,
                 const headers_t& headers, const std::string& version) :
                 http_entity_t(version, headers, body), method(method), path(path), query(query) {
  }

 http_request_t::info_t http_request_t::to_info(uint64_t id) const {
    auto connection_header = headers.find("Connection");
    auto do_not_track_header = headers.find("DNT");
    return info_t {
      id,
      static_cast<uint64_t>(version == "HTTP/1.0" ? 0 : 1),
      static_cast<uint64_t>(connection_header != headers.end() && connection_header->second == "Keep-Alive"),
      static_cast<uint64_t>(connection_header != headers.end() && connection_header->second == "Close"),
      static_cast<uint64_t>(do_not_track_header != headers.end() && do_not_track_header->second == "1"),
    };
  }

  std::string http_request_t::to_string() const {
    return to_string(method, path, body, query, headers, version);
  }

  std::string http_request_t::to_string(const method_t& method, const std::string& path, const std::string& body, const query_t& query,
                               const headers_t& headers, const std::string& version) {
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
    request += "\r\n";

    //headers
    for(const auto& header : headers) {
      if(header.first == "Content-Length")
        continue;
      request += header.first;
      request += ": ";
      request += header.second;
      request += "\r\n";
    }

    //body
    if(body.size()) {
      request += "Content-Length: ";
      request += std::to_string(body.size());
      request += "\r\n\r\n";
      request += body;
    }
    else
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

  query_t http_request_t::split_path_query(std::string& path) {
    //check for a query bit
    auto query_begin = path.find("?");
    auto key_start = path.begin() + query_begin + 1, value_start = path.end();
    query_t query;
    auto kv = query.end();
    //split up the keys and values
    for(auto c = key_start; c != path.end(); ++c) {
      switch(*c) {
        case '&':
          //we have a key where we can put this value
          if(kv != query.end())
            kv->second.emplace_back(std::string(value_start, c));
          //there mustn't have been a value
          else
            query[std::string(key_start, c)].emplace_back();
          //next key
          key_start = c + 1;
          kv = query.end();
          break;
        case '=':
          //no place to store a value yet
          if(kv == query.end()) {
            //so make one
            std::string key(key_start, c);
            query[key];
            kv = query.find(key);
            //value is here
            value_start = c + 1;
          }
          break;
      }
    }
    //get the last one
    if(key_start < path.end()) {
      if(kv != query.end())
        kv->second.emplace_back(std::string(value_start, path.end()));
      else
        query[std::string(key_start, path.end())].emplace_back();
    }
    //truncate the path
    if(query_begin != std::string::npos)
      path.resize(query_begin);
    return query;
  }

  std::list<http_request_t> http_request_t::from_stream(const char* start, size_t length, size_t max_size) {
    std::list<http_request_t> requests;
    cursor = start;
    end = start + length;
    while(start != end) {
      //bail if we've seen too much
      if(consumed + partial_buffer.size() + body_length > max_size)
        throw RESPONSE_413;

      //grab up to the next delimiter
      if(!consume_until()) {
        //should we have seen a method by now?
        if(state == METHOD && partial_buffer.size() >= METHOD_MAX_SIZE)
          throw RESPONSE_400;
        if(state == VERSION && partial_buffer.size() >= VERSION_MAX_SIZE)
          throw RESPONSE_400;
        break;
      }

      //what are we looking to parse
      switch(state) {
        case METHOD: {
          auto itr = STRING_TO_METHOD.find(partial_buffer);
          if(itr == STRING_TO_METHOD.end())
            throw RESPONSE_501;
          log_line = partial_buffer + delimiter;
          method = itr->second;
          state = PATH;
          break;
        }
        case PATH: {
          log_line += partial_buffer + delimiter;
          path = url_decode(partial_buffer);
          delimiter = "\r\n";
          state = VERSION;
          break;
        }
        case VERSION: {
          auto itr = SUPPORTED_VERSIONS.find(partial_buffer);
          if(itr == SUPPORTED_VERSIONS.end())
            throw RESPONSE_505;
          log_line += partial_buffer;
          version.swap(partial_buffer);
          query = split_path_query(path);
          state = HEADERS;
          break;
        }
        case HEADERS: {
          //a header is here
          if(partial_buffer.size()) {
            auto pos = partial_buffer.find(": ");
            if(pos == std::string::npos)
              throw RESPONSE_400;
            headers.insert({partial_buffer.substr(0, pos), partial_buffer.substr(pos + 2)});
          }//the end or body
          else {
            //standard length specified
            headers_t::const_iterator value;
            if((value = headers.find("Content-Length")) != headers.end()) {
              try{ body_length = std::stoul(value->second); }
              catch(...) { throw RESPONSE_400; }
              state = BODY;
            }//streaming chunks
            else if((value = headers.find("Transfer-Encoding")) != headers.end() && value->second == "chunked") {
              state = CHUNKS;
            }//simple GET
            else {
              requests.emplace_back(method, path, body, query, headers, version);
              requests.back().log_line.swap(log_line);
              flush_stream();
            }
          }
          break;
        }
        case BODY: {
          body.swap(partial_buffer);
          requests.emplace_back(method, path, body, query, headers, version);
          requests.back().log_line.swap(log_line);
          flush_stream();
          break;
        }
        case CHUNKS: {
          //TODO: actually parse out the length and chunk by alternating
          throw RESPONSE_501;
        }
      }

      //next piece
      partial_buffer.clear();
    }

    return requests;
  }
  void http_request_t::flush_stream() {
    http_entity_t::flush_stream(METHOD);
    path.clear();
    query.clear();
  }

  size_t http_request_t::size() const {
    return consumed + partial_buffer.size();
  }

  http_response_t::http_response_t():http_entity_t("", headers_t{}, ""){
    message = "";
    flush_stream();
  }

  http_response_t::http_response_t(unsigned code, const std::string& message, const std::string& body, const headers_t& headers,
                  const std::string& version) :
                  http_entity_t(version, headers, body), code(code), message(message) {
  }

  void http_response_t::from_info(http_request_t::info_t& info) {
    version = info.version ? "HTTP/1.1" : "HTTP/1.0";
    if(info.connection_keep_alive)
      headers.emplace("Connection", "Keep-Alive");
    if(info.connection_close)
      headers.emplace("Connection", "Close");
    //its useful to let the person getting this what the return code ended up being
    info.response_code = code;
  }

  std::string http_response_t::to_string() const {
    return generic(code, message, headers, body, version);
  }

  http_response_t http_response_t::from_string(const char* start, size_t length) {
    http_response_t response;
    auto responses = response.from_stream(start, length);
    if(responses.size() == 0)
      throw std::runtime_error("Incomplete http request");
    return std::move(responses.front());
  }

  std::list<http_response_t> http_response_t::from_stream(const char* start, size_t length) {
    std::list<http_response_t> responses;
    cursor = start;
    end = start + length;
    while(start != end) {
      //grab up to the next return
      if(!consume_until())
        break;

      //what are we looking to parse
      switch(state) {
        case MESSAGE: {
          //log_line = partial_buffer + delimiter;
          message.swap(partial_buffer);
          state = HEADERS;
          break;
        }
        case CODE: {
          //log_line += partial_buffer + delimiter;
          code = static_cast<uint16_t>(std::stoul(partial_buffer));
          delimiter = "\r\n";
          state = MESSAGE;
          break;
        }
        case VERSION: {
          if(SUPPORTED_VERSIONS.find(partial_buffer) == SUPPORTED_VERSIONS.end())
            throw std::runtime_error("Unknown http version");
          //log_line += partial_buffer;
          version.swap(partial_buffer);
          state = CODE;
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
              responses.emplace_back(code, message, body, headers, version);
              flush_stream();
            }
          }
          break;
        }
        case BODY: {
          body.swap(partial_buffer);
          responses.emplace_back(code, message, body, headers, version);
          flush_stream();
          break;
        }
        case CHUNKS: {
          //TODO: actually parse out the length and chunk by alternating
          //TODO: return 501
          throw std::runtime_error("not implemented");
        }
      }

      //next piece
      partial_buffer.clear();
    }

    return responses;
  }

  void http_response_t::flush_stream() {
    http_entity_t::flush_stream(VERSION);
    message.clear();
  }

  std::string http_response_t::generic(unsigned code, const std::string& message, const headers_t& headers, const std::string& body, const std::string& version) {
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
    //TODO: content length is optional
    //with 1.0 the end can be signaled by socket close
    //with 1.1 you can omit it when using chunked encoding
    response += "Content-Length: ";
    response += std::to_string(body.size());
    response += "\r\n\r\n";
    response += body;
    return response;
  }

  http_server_t::http_server_t(zmq::context_t& context, const std::string& client_endpoint, const std::string& proxy_endpoint, const std::string& result_endpoint, bool log, size_t max_request_size):
    server_t<http_request_t, http_request_t::info_t>::server_t(context, client_endpoint, proxy_endpoint, result_endpoint, log, max_request_size), request_id(0) {
  }
  http_server_t::~http_server_t(){}

  bool http_server_t::enqueue(const void* message, size_t size, const std::string& requester, http_request_t& request) {
    //do some parsing
    std::list<http_request_t> parsed_requests;
    try {
      parsed_requests = request.from_stream(static_cast<const char*>(message), size, max_request_size);
    }//something went wrong, either in parsing or size limitation
    catch(const request_exception_t& e) {
      client.send(requester, ZMQ_SNDMORE | ZMQ_DONTWAIT);
      client.send(e.response, ZMQ_DONTWAIT);
      if(log) {
        log_request(request_id, request.log_line);
        log_response(request_id, e.code, e.response.size());
      }
      ++request_id;
      return false;
    }

    //send on each request
    for(const auto& parsed_request : parsed_requests) {
      //figure out if we are expecting to close this request or not
      auto info = parsed_request.to_info(request_id);
      //send on the request
      this->proxy.send(requester, ZMQ_DONTWAIT | ZMQ_SNDMORE);
      this->proxy.send(static_cast<const void*>(&info), sizeof(info), ZMQ_DONTWAIT | ZMQ_SNDMORE);
      this->proxy.send(parsed_request.to_string(), ZMQ_DONTWAIT);
      if(log)
        log_request(request_id, parsed_request.log_line);
      //remember we are working on it
      this->requests.emplace(request_id++, requester);
    }
    return true;
  }
  void http_server_t::dequeue(const http_request_t::info_t& request_info, size_t length) {
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
        log_response(request_info.id, request_info.response_code, length);
    }
    else
      logging::WARN("Unknown or timed-out request id: " + std::to_string(request_info.id));
  }
}
