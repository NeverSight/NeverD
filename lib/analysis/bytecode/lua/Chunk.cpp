//===- Chunk.cpp - Bounded Lua record reads ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// All byte ranges and allocation charges are checked before use.
//===----------------------------------------------------------------------===//

#include "Chunk.h"

#include <charconv>
#include <cmath>
#include <limits>

namespace neverd::lua {
llvm::StringRef Reader::take(uint64_t N) {
  Job.check(at());
  if (N > left())
    malformed(at(), "Truncated Lua record");
  auto Part = Bytes.substr(Pos, N);
  Pos += N;
  return Part;
}
Reader Reader::slice(size_t N) {
  size_t Start = at();
  return {take(N), Job, 0, Start, Big};
}
bool Reader::boolean() {
  auto V = byte();
  if (V > 1)
    malformed(at() - 1, "Invalid boolean");
  return V;
}
uint64_t Reader::word(unsigned W) {
  auto Part = take(W);
  uint64_t V = 0;
  for (unsigned I = 0; I < W; ++I)
    V |= uint64_t(uint8_t(Part[Big ? W - I - 1 : I])) << (8 * I);
  return V;
}
uint64_t Reader::varint(bool Little, bool HighTerminates, unsigned Bits) {
  uint64_t V = 0, Max = std::numeric_limits<uint64_t>::max() >> (64 - Bits);
  size_t Start = at();
  for (unsigned Shift = 0; Shift < Bits; Shift += 7) {
    unsigned B = byte(), Part = B & 127;
    if (Little ? Part > (Max >> Shift) : V > (Max >> 7))
      break;
    V = Little ? V | (uint64_t(Part) << Shift) : (V << 7) | Part;
    if (bool(B & 128) == HighTerminates)
      return V;
  }
  malformed(Start, "Variable-length integer exceeds field width");
}
unsigned Reader::count(uint64_t N) {
  Job.use(Job.Items, N, at());
  return static_cast<unsigned>(N);
}
void Reader::align(unsigned Width) {
  auto Pad = take((Width - at() % Width) % Width);
  if (Pad.find_first_not_of('\0') != llvm::StringRef::npos)
    malformed(at() - Pad.size(), "Invalid alignment padding");
}
void Reader::expect(llvm::StringRef Literal) {
  if (take(Literal.size()) != Literal)
    malformed(at() - Literal.size(),
              "Invalid format signature or auxiliary word");
}
void Reader::end() {
  if (left())
    malformed(at(), "Unexpected trailing data");
}
Proto &Chunk::proto(Reader &R) {
  Job.use(Job.Items, 1, R.at());
  auto &P = Functions.emplace_back();
  P.Id = Functions.size() - 1;
  P.Start = R.at();
  return P;
}
void Chunk::code(Proto &P, Reader &R, uint64_t N) {
  Job.use(Job.Words, N, R.at());
  if (!N || N > R.left() / 4)
    malformed(R.at(), "Empty or truncated instruction stream");
  if (P.Params > P.Stack)
    malformed(R.at(), "Parameter count exceeds frame size");
  P.CodeAt = R.at();
  P.Code.reserve(N);
  for (unsigned I = 0; I < N; ++I)
    P.Code.push_back(R.word(4));
}
std::string quote(llvm::StringRef S) {
  std::string Out = "\"";
  for (unsigned char C : S) {
    if (C == '"' || C == '\\') {
      Out += '\\';
      Out += char(C);
    } else if (C >= 32 && C < 127)
      Out += char(C);
    else {
      Out += '\\';
      Out += char('0' + C / 100);
      Out += char('0' + C / 10 % 10);
      Out += char('0' + C % 10);
    }
  }
  return Out + '"';
}
std::string number(double F, bool FloatSubtype) {
  if (!std::isfinite(F))
    return {};
  char Buffer[64];
  auto End = std::to_chars(Buffer, Buffer + sizeof(Buffer), F,
                           std::chars_format::general,
                           std::numeric_limits<double>::max_digits10);
  std::string S(Buffer, End.ptr);
  if (FloatSubtype && S.find_first_of(".eE") == std::string::npos)
    S += ".0";
  return S;
}
std::string integer(int64_t I) {
  // The magnitude of INT64_MIN by itself is a float literal in Lua.
  return I == INT64_MIN ? "(-9223372036854775807 - 1)" : std::to_string(I);
}
} // namespace neverd::lua
