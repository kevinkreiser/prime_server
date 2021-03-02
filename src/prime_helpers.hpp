#pragma once

#include <string>
#include <utility>
#include <vector>

template <typename T = std::string>
std::vector<T> split(
    const std::string& s,
    char delim,
    bool skip_empty = true,
    const std::function<T(const std::string&)>& transform = [](const std::string& s) { return s; }) {
  std::vector<T> parts;
  std::string::size_type previous = 0, current;
  while ((current = s.find(previous, delim)) != std::string::npos) {
    auto part = s.substr(previous, current - previous);
    if (!part.empty() || !skip_empty)
      parts.emplace_back(transform(part));
    previous = current;
  }
  auto part = s.substr(previous, current - previous);
  if (!part.empty() || !skip_empty)
    parts.emplace_back(transform(part));
  return parts;
}

std::pair<unsigned int, unsigned int> parse_quiesce_config(const std::string& config,
                                                           unsigned int drain_seconds = 28,
                                                           unsigned int shutdown_seconds = 1) {
  auto parts = split<unsigned int>(config, ',', true, [](const std::string& s) -> unsigned int {
    return std::stoul(s);
  });
  return std::pair<unsigned int, unsigned int>(parts.size() > 0 ? parts[0] : drain_seconds,
                                               parts.size() > 1 ? parts[1] : shutdown_seconds);
}
