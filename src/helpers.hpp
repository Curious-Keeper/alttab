#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
inline std::string middleTruncate(std::string str, size_t maxLen = 40) {
  if (str.length() <= maxLen)
    return str;
  maxLen = std::max((int)maxLen, 5);

  size_t sideLen = (maxLen - 3) / 2;
  return str.substr(0, sideLen) + "..." + str.substr(str.length() - sideLen);
}

inline std::string toLower(std::string_view str) {
  std::string out(str);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return out;
}
