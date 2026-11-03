#pragma once
#include <cstddef>

#include <string_view>

constexpr const char* operator""_g(const char* key, size_t size) noexcept {
  auto sKey = std::string_view{key, size};
  if(sKey == "github") {
    return "https://github.com/Shmarkus/Sourcetrail";
  }
  if(sKey == "documentation") {
    return "https://github.com/Shmarkus/Sourcetrail/blob/master/DOCUMENTATION.md";
  }
  return "";
}