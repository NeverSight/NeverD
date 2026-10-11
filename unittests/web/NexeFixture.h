//===- NexeFixture.h - Inert nexe envelope fixtures ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independently constructed container bytes; never execute a target runtime.
///
//===----------------------------------------------------------------------===//

#pragma once

#include <bit>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace neverd::web::nexe_test {
inline void put(std::string &B, uint64_t At, uint64_t Bits) {
  if (At > B.size() || B.size() - At < 8)
    throw std::out_of_range("fixture");
  for (unsigned I = 0; I < 8; ++I)
    B[At + I] = char(Bits >> (8 * I));
}
inline std::string bootstrap(std::string_view Index) {
  return "!(function () {process.__nexe = " + std::string(Index) +
         ";\n})();;/* CANARY_BOOTSTRAP */";
}
inline std::string envelope(std::string_view Code, std::string_view Resources,
                            std::string_view Prefix = "CANARY_OPAQUE_PREFIX") {
  std::string B(Prefix);
  B += Code;
  B += Resources;
  B += "<nexe~~sentinel>";
  B.resize(B.size() + 16);
  put(B, B.size() - 16, std::bit_cast<uint64_t>(double(Code.size())));
  put(B, B.size() - 8, std::bit_cast<uint64_t>(double(Resources.size())));
  return B;
}
inline std::string image(std::string_view Index, std::string_view Resources,
                         std::string_view Prefix = "CANARY_OPAQUE_PREFIX") {
  return envelope(bootstrap(Index), Resources, Prefix);
}
inline std::string
bundle(const std::vector<std::pair<std::string, std::string>> &Files,
       std::string_view Prefix = "CANARY_OPAQUE_PREFIX") {
  std::string Index = "{\"resources\":{", Resources;
  for (const auto &[Name, Bytes] : Files) {
    if (Name.find_first_of("\"\\\r\n") != Name.npos)
      throw std::invalid_argument("fixture needs a literal portable key");
    if (Index.back() != '{')
      Index += ',';
    Index += "\"./" + Name + "\":[" + std::to_string(Resources.size()) + "," +
             std::to_string(Bytes.size()) + "]";
    Resources += Bytes;
  }
  return image(Index + "}}", Resources, Prefix);
}
} // namespace neverd::web::nexe_test
