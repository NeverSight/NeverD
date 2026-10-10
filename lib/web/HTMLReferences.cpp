//===- HTMLReferences.cpp - HTML character references ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// HTML character references.
///
//===----------------------------------------------------------------------===//

#include "neverd/web/Error.h"
#include "neverd/web/HTML.h"

#include <algorithm>
#include <iterator>

namespace neverd::web {
namespace {
struct NamedReference {
  std::string_view Name, Text;
};
#include "HTMLNamedReferences.inc"
bool alpha(char C) { return (C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z'); }
bool digit(char C) { return C >= '0' && C <= '9'; }
unsigned hex(char C) {
  if (digit(C))
    return C - '0';
  if (C >= 'a' && C <= 'f')
    return C - 'a' + 10;
  if (C >= 'A' && C <= 'F')
    return C - 'A' + 10;
  return 16;
}
void append(std::string &Out, uint32_t C) {
  if (C < 0x80)
    Out += static_cast<char>(C);
  else if (C < 0x800) {
    Out += static_cast<char>(0xc0 | (C >> 6));
    Out += static_cast<char>(0x80 | (C & 63));
  } else if (C < 0x10000) {
    Out += static_cast<char>(0xe0 | (C >> 12));
    Out += static_cast<char>(0x80 | ((C >> 6) & 63));
    Out += static_cast<char>(0x80 | (C & 63));
  } else {
    Out += static_cast<char>(0xf0 | (C >> 18));
    Out += static_cast<char>(0x80 | ((C >> 12) & 63));
    Out += static_cast<char>(0x80 | ((C >> 6) & 63));
    Out += static_cast<char>(0x80 | (C & 63));
  }
}
} // namespace
std::string decodeHTMLAttribute(std::string_view Bytes, uint64_t &Steps,
                                uint64_t &Decoded) {
  if (Bytes.size() > MaxHTMLAttributeBytes)
    throw Error("html_attribute_budget_exceeded");
  const auto Step = [&](uint64_t N = 1) {
    if (Steps > MaxHTMLSteps || N > MaxHTMLSteps - Steps)
      throw Error("html_work_budget_exceeded");
    Steps += N;
  };
  std::string Out;
  for (size_t I = 0; I < Bytes.size();) {
    Step();
    const auto C = Bytes[I++];
    if (C == '\r') {
      Out += '\n';
      if (I < Bytes.size() && Bytes[I] == '\n')
        ++I;
      continue;
    }
    if (C != '&') {
      Out += C;
      continue;
    }
    const auto Start = I;
    if (I < Bytes.size() && Bytes[I] == '#') {
      ++I;
      const unsigned Base =
          I < Bytes.size() && (Bytes[I] == 'x' || Bytes[I] == 'X') ? (++I, 16)
                                                                   : 10;
      const auto Digits = I;
      uint32_t Value = 0;
      for (; I < Bytes.size() && hex(Bytes[I]) < Base; ++I) {
        Step();
        Value =
            Value > 0x10ffff / Base ? 0x110000 : Value * Base + hex(Bytes[I]);
      }
      if (I == Digits) {
        I = Start;
        Out += '&';
        continue;
      }
      if (I < Bytes.size() && Bytes[I] == ';')
        ++I;
      if (!Value || Value > 0x10ffff || (Value >= 0xd800 && Value <= 0xdfff))
        Value = 0xfffd;
      static constexpr uint32_t Remap[] = {
          0x20ac, 0x81,   0x201a, 0x192,  0x201e, 0x2026, 0x2020, 0x2021,
          0x2c6,  0x2030, 0x160,  0x2039, 0x152,  0x8d,   0x17d,  0x8f,
          0x90,   0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
          0x2dc,  0x2122, 0x161,  0x203a, 0x153,  0x9d,   0x17e,  0x178};
      if (Value >= 0x80 && Value <= 0x9f)
        Value = Remap[Value - 0x80];
      append(Out, Value);
      continue;
    }
    size_t End = I;
    while (End < Bytes.size() && End - I < 32 &&
           (alpha(Bytes[End]) || digit(Bytes[End]))) {
      Step();
      ++End;
    }
    if (End < Bytes.size() && Bytes[End] == ';' && End - I < 32)
      ++End;
    const NamedReference *Found = nullptr;
    for (size_t Length = End - I; Length; --Length) {
      const auto Name = Bytes.substr(I, Length);
      const auto It = std::lower_bound(
          std::begin(NamedReferences), std::end(NamedReferences), Name,
          [&](const auto &Entry, std::string_view Key) {
            Step(std::min(Entry.Name.size(), Key.size()) + 1);
            return Entry.Name < Key;
          });
      Step(Name.size() + 1);
      if (It != std::end(NamedReferences) && It->Name == Name) {
        Found = It;
        break;
      }
    }
    if (!Found) {
      Out += '&';
      continue;
    }
    const auto After = I + Found->Name.size();
    if (Found->Name.back() != ';' && After < Bytes.size() &&
        (alpha(Bytes[After]) || digit(Bytes[After]) || Bytes[After] == '=')) {
      Out += '&';
      continue;
    }
    Out += Found->Text;
    I = After;
  }
  if (Decoded > MaxHTMLDecodedBytes ||
      Out.size() > MaxHTMLDecodedBytes - Decoded)
    throw Error("html_decoded_byte_budget_exceeded");
  Decoded += Out.size();
  return Out;
}
} // namespace neverd::web
