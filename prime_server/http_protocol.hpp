#ifndef __HTTP_PROTOCOL_HPP__
#define __HTTP_PROTOCOL_HPP__

#include <prime_server/prime_server.hpp>
#include <prime_server/zmq_helpers.hpp>

#include <unordered_map>
#include <list>
#include <cstdint>
#include <string>

namespace prime_server {

  class http_client_t : public client_t {
   public:
    http_client_t(zmq::context_t& context, const std::string& server_endpoint, const request_function_t& request_function,
      const collect_function_t& collect_function, size_t batch_size = 8912);
   protected:
    virtual size_t stream_responses(const void* message, size_t size, bool& more);
    void reset();

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
  struct http_entity_t {
    std::string version;
    headers_t headers;
    std::string body;
    http_entity_t(const std::string& version, const headers_t& headers, const std::string& body);

    virtual ~http_entity_t();
    virtual std::string to_string() const = 0;
    //TODO: virtual http_entity_t from_string(const char*, size_t length) = 0
  };

  struct http_request_t : public http_entity_t {
   public:
    method_t method;
    std::string path;
    query_t query;

    http_request_t();
    http_request_t(const method_t& method, const std::string& path, const std::string& body = "", const query_t& query = query_t{},
                   const headers_t& headers = headers_t{}, const std::string& version = "HTTP/1.1");

    struct info_t {
      uint64_t id                         :32; //the request id
      uint64_t version                    :3;  //protocol specific space for versioning info
      uint64_t connection_keep_alive      :1;  //header present or not
      uint64_t connection_close           :1;  //header present or not
      uint64_t spare                      :27; //unused information
    };

    info_t to_info(uint64_t id) const;
    virtual std::string to_string() const;
    static std::string to_string(const method_t& method, const std::string& path, const std::string& body = "", const query_t& query = query_t{},
                                 const headers_t& headers = headers_t{}, const std::string& version = "HTTP/1.1");
    static http_request_t from_string(const char* start, size_t length);
    std::list<http_request_t> from_stream(const char* start, size_t length);
    void flush_stream();
    size_t size() const;


   protected:
    //state for streaming parsing
    const char *cursor, *end, *delimeter;
    std::string partial_buffer;
    size_t partial_length;
    enum state_t { METHOD, PATH, VERSION, HEADERS, BODY, CHUNKS, END };
    state_t state;
    size_t body_length;
    size_t consumed;

    bool consume_until();
  };

  //TODO: let this subclass exception and make 'message' be the 'what'
  //then the caught exceptions that we want to actuall return to the client
  //can be handled more easily
  struct http_response_t : public http_entity_t {
    unsigned code;
    std::string message;

    http_response_t(unsigned code, const std::string& message, const std::string& body = "", const headers_t& headers = headers_t{},
                    const std::string& version = "HTTP/1.1");

    void from_info(const http_request_t::info_t* info);

    virtual std::string to_string() const;

    static std::string generic(unsigned code, const std::string message, const headers_t& headers = headers_t{}, const std::string& body = "");
  };

  class http_server_t : public server_t<http_request_t, http_request_t::info_t> {
   public:
    http_server_t(zmq::context_t& context, const std::string& client_endpoint, const std::string& proxy_endpoint, const std::string& result_endpoint);
    virtual ~http_server_t();
   protected:
    virtual void enqueue(const void* message, size_t size, const std::string& requester, http_request_t& request);
    virtual void dequeue(const http_request_t::info_t& request_info);
   protected:
    uint64_t request_id;
  };

}

#endif //__HTTP_PROTOCOL_HPP__
