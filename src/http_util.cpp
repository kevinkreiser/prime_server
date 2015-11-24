#include "http_util.hpp"

#include <fstream>
#include <map>
#include <cctype>
#include <algorithm>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

using namespace prime_server::http;
namespace {
  std::unordered_map<std::string, header_t> load_mimes() {
    //hardcode some just in case we cant get some automatically
    std::unordered_map<std::string, header_t> mimes{
      {"htm", HTML_MIME},
      {"html", HTML_MIME},
      {"js", JS_MIME},
      {"json", JSON_MIME},
      {"jpg", JPEG_MIME},
      {"jpeg", JPEG_MIME},
    };

    //TODO: you can get some of these from /etc/mime.types

    return mimes;
  }

  std::string current_working_directory() {
    char* absolute_path = get_current_dir_name();
    std::string cwd(absolute_path);
    free(absolute_path);
    return cwd;
  }

  struct stat canonical_path(const std::string& cwd, std::string& path) {
    //yeah we only allow one dot at a time
    for(size_t p = 0, i = path.find('.', 0); i != std::string::npos; p = i, i = path.find('.', i))
      if(p + 1 == i)
        path[p] = path[i] = '/';
    //and this better be a regular file that exists
    struct stat s;
    if(stat((cwd + path).c_str(), &s))
      s.st_mode = 0;
    return s;
  }

}

namespace prime_server {
  namespace http {

    const header_t& mime_header(const std::string& file_name, const header_t& default_mime_header) {
      static const auto mimes(load_mimes());
      auto extension = file_name.substr(file_name.find_last_of('.') + 1);
      std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
      auto header = mimes.find(extension);
      if(header == mimes.cend())
        return default_mime_header;
      return header->second;
    }

    worker_t::result_t disk_result(const http_request_t& request, http_request_t::info_t& request_info, size_t size_limit, bool allow_listing) {
      worker_t::result_t result{false};
      //get the canonical path
      static const auto cwd(current_working_directory());
      auto path = request.path;
      for(size_t p = path.size(), i = path.find('.', 0); i != std::string::npos; p = i, i = path.find('.', i + 1))
        if(p + 1 == i)
          path[p] = path[i] = '/';
      auto canonical = cwd + path;
      //stat it
      struct stat s;
      if(stat(canonical.c_str(), &s))
        s.st_mode = 0;
      //a regular file
      if(s.st_size <= size_limit && (s.st_mode & S_IFREG)) {
        //have to be able to open it
        std::fstream input(canonical, std::ios::in | std::ios::binary);
        if(input) {
          std::string buffer((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
          http_response_t response(200, "OK", buffer, headers_t{CORS, mime_header(path)});
          response.from_info(request_info);
          result.messages = {response.to_string()};
          return result;
        }
      }//a directory
      else if(allow_listing && (s.st_mode & S_IFDIR)) {
        //loop over the directory contents
        auto* dir = opendir(canonical.c_str());
        struct dirent* entry;
        std::map<std::string, unsigned char> entries;
        while((entry = readdir(dir)) != nullptr)
          entries.emplace(entry->d_name, entry->d_type);
        closedir(dir);
        //generate the html
        std::string listing;
        for(const auto& entry : entries) {
          if(entry.second == DT_REG)
            listing += "<a href='" + entry.first + "'>" + entry.first + "</a><br/>";
          else if(entry.second == DT_DIR && entry.first != ".")
            listing += "<a href='" + entry.first + "/'>" + entry.first + "</a><br/>";
        }
        http_response_t response(200, "OK", listing, headers_t{CORS, HTML_MIME});
        response.from_info(request_info);
        result.messages = {response.to_string()};
        return result;
      }
      //didn't make the cut
      http_response_t response(404, "Not Found", "Not Found");
      response.from_info(request_info);
      result.messages = {response.to_string()};
    }

  }
}
