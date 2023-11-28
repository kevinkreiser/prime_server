#pragma once

#include <prime_server/http_protocol.hpp>
#include <prime_server/prime_server.hpp>

#include <string>
#include <unordered_map>

namespace prime_server {
namespace http {

using header_t = std::unordered_map<std::string, std::string>::value_type;
const header_t CORS{"Access-Control-Allow-Origin", "*"};
const header_t KEEP_ALIVE{"Connection", "Keep-Alive"};
const header_t CLOSE{"Connection", "CLOSE"};
const header_t DEFAULT_MIME{"Content-type", "application/octet-stream"};
const header_t HTML_MIME{"Content-type", "text/html"};
const header_t JS_MIME{"Content-type", "application/javascript"};
const header_t JSON_MIME{"Content-type", "application/json"};
const header_t JPEG_MIME{"Content-type", "image/jpeg"};

// get a mime header for the given file name
const header_t& mime_header(const std::string& file_name,
                            const header_t& default_mime_header = DEFAULT_MIME);

// get a static file or directory listing
worker_t::result_t disk_result(const http_request_t& path,
                               http_request_info_t& request_info,
                               const std::string& root = "./",
                               bool allow_listing = true,
                               size_t size_limit = 1024 * 1024 * 1024);

uint8_t get_method_mask(const std::string& verb_list);

} // namespace http
} // namespace prime_server
