#include "netstring_protocol.hpp"
#include "logging/logging.hpp"

#include <cctype>
#include <cstdlib>
#include <ctime>

using namespace prime_server;

namespace {

const netstring_entity_t::request_exception_t BAD_LENGTH("BAD_REQUEST: Non-numeric length");
const netstring_entity_t::request_exception_t
    TOO_LONG("BAD_REQUEST: Request exceeded maximum length");
const netstring_entity_t::request_exception_t
    BAD_BODY_SEPARATOR("BAD_REQUEST: Missing ':' between length and body");
const netstring_entity_t::request_exception_t
    BAD_MESSAGE_SEPARATOR("BAD_REQUEST: Missing ',' after body");

} // namespace

namespace prime_server {

netstring_entity_t::netstring_entity_t() : body(), body_length(0) {
}

netstring_request_info_t netstring_entity_t::to_info(uint32_t id) const {
  return netstring_request_info_t{id, static_cast<uint32_t>(difftime(time(nullptr), 0) + .5)};
}

std::string netstring_entity_t::to_string() const {
  return std::to_string(body.size()) + ':' + body + ',';
}

std::string netstring_entity_t::to_string(const std::string& body) {
  return std::to_string(body.size()) + ':' + body + ',';
}

netstring_entity_t netstring_entity_t::from_string(const char* start, size_t length) {
  netstring_entity_t request;
  auto requests = request.from_stream(start, length);
  if (requests.size() == 0)
    throw std::runtime_error("Incomplete netstring request");
  return std::move(requests.front());
}

const zmq::message_t& netstring_entity_t::timeout(netstring_request_info_t&) {
  static char TIMEOUT[] = "7:TIMEOUT,";
  static const zmq::message_t t(static_cast<void*>(&TIMEOUT[0]), sizeof(TIMEOUT) - 1,
                                [](void*, void*) {});
  return t;
}

std::list<netstring_entity_t>
netstring_entity_t::from_stream(const char* start, size_t length, size_t max_size) {
  std::list<netstring_entity_t> requests;
  size_t i = 0;
  size_t remaining = 0;
  while (i < length) {
    // we are looking for the length
    if (body_length == 0) {
      // if its not a digit then we need to see what it is
      auto c = start[i];
      if (!std::isdigit(c)) {
        // found the end of the length
        if (c == ':') {
          // get the length
          body.append(start, start + i);
          try {
            body_length = std::stoul(body);
          } catch (...) { throw BAD_LENGTH; }
          if (body_length > max_size)
            throw TOO_LONG;
          // reset for body
          start += i + 1;
          length -= i + 1;
          i = 0;
          body.clear();
          continue;
        } // this isnt what we expect
        else if (i > 0)
          throw BAD_BODY_SEPARATOR;
        else
          throw BAD_LENGTH;
      }
      // next char
      ++i;
      continue;
    } // we are looking for the body
    else if ((remaining = body_length - body.size()) < length) {
      if (start[remaining] == ',') {
        // make a new one
        requests.emplace_back();
        requests.back().body = std::move(body);
        requests.back().body_length = 0;
        requests.back().body.append(start, start + remaining);
        // reset for next
        start += remaining + 1;
        length -= remaining + 1;
        i = 0;
        flush_stream();
        continue;
      } // this isnt what we expect
      else
        throw BAD_MESSAGE_SEPARATOR;
    }
    // we werent looking for length, and the body didnt fit in what we had
    // so we are done for now and need more data
    break;
  }
  // whatever is left we want to keep, it could be nothing
  body.append(start, start + length);
  return requests;
}

void netstring_entity_t::flush_stream() {
  body.clear();
  body_length = 0;
}

size_t netstring_entity_t::size() const {
  // TODO: pedantic, comma, colon and length are part of the request
  return body.size();
}

void netstring_entity_t::log(uint32_t id) const {
  auto line = std::to_string(id);
  line.reserve(line.size() + body.size() + 64);
  line.push_back(' ');
  line.append(logging::timestamp());
  line.push_back(' ');
  line.append(body);
  line.push_back('\n');
  logging::log(line);
}

netstring_entity_t::request_exception_t::request_exception_t(const std::string& response)
    : response(netstring_entity_t::to_string(response)) {
}

void netstring_entity_t::request_exception_t::log(uint32_t id) const {
  auto line = std::to_string(id);
  line.reserve(line.size() + 64);
  line.push_back(' ');
  line.append(logging::timestamp());
  line.append(" BAD_REQ ");
  line.append(std::to_string(response.size()));
  line.push_back('\n');
  logging::log(line);
}

void netstring_request_info_t::log(size_t response_size) const {
  auto line = std::to_string(id);
  line.reserve(line.size() + 64);
  line.push_back(' ');
  line.append(logging::timestamp());
  line.append(" OK_RESP ");
  line.append(std::to_string(response_size));
  line.push_back('\n');
  logging::log(line);
}

size_t netstring_client_t::stream_responses(const void* message, size_t size, bool& more) {
  auto responses = response.from_stream(static_cast<const char*>(message), size);
  for (const auto& parsed_response : responses) {
    // TODO: this is wasteful
    auto formatted_response = netstring_entity_t::to_string(parsed_response.body);
    more = collect_function(static_cast<const void*>(formatted_response.c_str()),
                            formatted_response.size());
  }
  return responses.size();
}

shortcircuiter_t<netstring_entity_t> make_shortcircuiter(const std::string& health_check_str) {
  netstring_entity_t response = netstring_entity_t::from_string("healthy", 7);
  std::string response_str = response.to_string();
  std::shared_ptr<zmq::message_t> shared_response = std::make_shared<zmq::message_t>(response_str.size(), response_str.c_str());

  return [=](const netstring_entity_t& request) {
    if (request.body == health_check_str) {
      return shared_response;
    }
    return std::shared_ptr<zmq::message_t>(nullptr);
  };
}
} // namespace prime_server
