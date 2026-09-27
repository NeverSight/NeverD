//===- MobileDalvikIdentity.h - Canonical Dalvik member text -----------===//
#pragma once

#include "MobileDalvik.h"

#include <string>
#include <vector>

namespace neverd::mobile::dalvik::detail {
inline unsigned incomingWords(const std::vector<std::string> &parameters,
                              bool is_static) {
  uint64_t result = !is_static;
  for (const auto &parameter : parameters)
    result += width(parameter);
  if (result > 65535)
    throw Error("Dalvik incoming register frame exceeds its word limit");
  return static_cast<unsigned>(result);
}
inline std::string methodSignature(const std::vector<std::string> &parameters,
                                   const std::string &returns) {
  std::string result = "(";
  for (const auto &parameter : parameters)
    result += parameter;
  return result + ")" + returns;
}
inline std::string methodIdentity(const std::string &owner,
                                  const std::string &name,
                                  const std::vector<std::string> &parameters,
                                  const std::string &returns) {
  return owner + "->" + name + methodSignature(parameters, returns);
}
} // namespace neverd::mobile::dalvik::detail
