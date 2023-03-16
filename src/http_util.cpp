#include "http_util.hpp"

#include <algorithm>
#include <cctype>
#include <dirent.h>
#include <fstream>
#include <map>
#include <sys/stat.h>
#include <unistd.h>
#include <string>
#include <sstream>

using namespace prime_server::http;
namespace {

std::unordered_map<std::string, header_t> load_mimes() {
  // hardcode some just in case we cant get some automatically
  std::unordered_map<std::string, header_t> mimes{
      {"htm", HTML_MIME},  {"html", HTML_MIME}, {"js", JS_MIME},
      {"json", JSON_MIME}, {"jpg", JPEG_MIME},  {"jpeg", JPEG_MIME},
  };

  // TODO: you can get some of these from /etc/mime.types

  return mimes;
}

} // namespace

namespace prime_server {
namespace http {

const header_t& mime_header(const std::string& file_name, const header_t& default_mime_header) {
  static const auto mimes(load_mimes());
  auto extension = file_name.substr(file_name.find_last_of('.') + 1);
  std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
  auto header = mimes.find(extension);
  if (header == mimes.cend())
    return default_mime_header;
  return header->second;
}

worker_t::result_t disk_result(const http_request_t& request,
                               http_request_info_t& request_info,
                               const std::string& root,
                               bool allow_listing,
                               size_t size_limit) {
  worker_t::result_t result{false, {}, {}};
  // get the canonical path
  auto path = request.path;
  for (size_t p = path.size(), i = path.find('.', 0); i != std::string::npos;
       p = i, i = path.find('.', i + 1))
    if (p + 1 == i)
      path[p] = path[i] = '/';
  auto canonical = root + path;
  // stat it
  struct stat s;
  if (stat(canonical.c_str(), &s))
    s.st_mode = 0;
  // a regular file
  if (static_cast<size_t>(s.st_size) <= size_limit && (s.st_mode & S_IFREG)) {
    // have to be able to open it
    std::fstream input(canonical, std::ios::in | std::ios::binary);
    if (input) {
      std::string buffer((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
      http_response_t response(200, "OK", buffer, headers_t{CORS, mime_header(path)});
      response.from_info(request_info);
      result.messages = {response.to_string()};
    }
  } // a directory
  else if (allow_listing && (s.st_mode & S_IFDIR)) {
    // loop over the directory contents
    auto* dir = opendir(canonical.c_str());
    struct dirent* entry;
    std::map<std::string, unsigned char> entries;
    while ((entry = readdir(dir)) != nullptr)
      entries.emplace(entry->d_name, entry->d_type);
    closedir(dir);
    // generate the html
    std::string listing;
    for (const auto& entry : entries) {
      if (entry.second == DT_REG)
        listing += "<a href='" + entry.first + "'>" + entry.first + "</a><br/>";
      else if (entry.second == DT_DIR && entry.first != ".")
        listing += "<a href='" + entry.first + "/'>" + entry.first + "</a><br/>";
    }
    http_response_t response(200, "OK", listing, headers_t{CORS, HTML_MIME});
    response.from_info(request_info);
    result.messages = {response.to_string()};
  } // didn't make the cut
  else {
    http_response_t response(404, "Not Found", "Not Found");
    response.from_info(request_info);
    result.messages = {response.to_string()};
  }
  // hand it back
  return result;
}


uint8_t get_method_mask(const std::string& verb_list) {
  uint8_t method_mask = 0;
  std::istringstream iss(verb_list);
  std::string verb;

  while (std::getline(iss, verb, ',')) {
    auto it = STRING_TO_METHOD.find(verb);
    if (it != STRING_TO_METHOD.end()) {
      method_mask |= it->second;
    }
  }

  return method_mask;
}

} // namespace http
} // namespace prime_server
