//===- SHA512.cpp - Streaming package digests --------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// SHA-384/512 from FIPS 180-4 sections 4.1.3, 4.2.3, 5 and 6.4/6.5.
/// This implementation is bounded by the captured-input byte limit.
///
//===----------------------------------------------------------------------===//

#include "SHA512.h"

#include "neverd/web/Error.h"
#include "neverd/web/Limits.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace neverd::web {
namespace {
// FIPS 180-4, section 4.2.3: fractional cube roots of the first 80 primes.
constexpr std::array<uint64_t, 80> RoundConstants{
    0x428a2f98d728ae22, 0x7137449123ef65cd, 0xb5c0fbcfec4d3b2f,
    0xe9b5dba58189dbbc, 0x3956c25bf348b538, 0x59f111f1b605d019,
    0x923f82a4af194f9b, 0xab1c5ed5da6d8118, 0xd807aa98a3030242,
    0x12835b0145706fbe, 0x243185be4ee4b28c, 0x550c7dc3d5ffb4e2,
    0x72be5d74f27b896f, 0x80deb1fe3b1696b1, 0x9bdc06a725c71235,
    0xc19bf174cf692694, 0xe49b69c19ef14ad2, 0xefbe4786384f25e3,
    0x0fc19dc68b8cd5b5, 0x240ca1cc77ac9c65, 0x2de92c6f592b0275,
    0x4a7484aa6ea6e483, 0x5cb0a9dcbd41fbd4, 0x76f988da831153b5,
    0x983e5152ee66dfab, 0xa831c66d2db43210, 0xb00327c898fb213f,
    0xbf597fc7beef0ee4, 0xc6e00bf33da88fc2, 0xd5a79147930aa725,
    0x06ca6351e003826f, 0x142929670a0e6e70, 0x27b70a8546d22ffc,
    0x2e1b21385c26c926, 0x4d2c6dfc5ac42aed, 0x53380d139d95b3df,
    0x650a73548baf63de, 0x766a0abb3c77b2a8, 0x81c2c92e47edaee6,
    0x92722c851482353b, 0xa2bfe8a14cf10364, 0xa81a664bbc423001,
    0xc24b8b70d0f89791, 0xc76c51a30654be30, 0xd192e819d6ef5218,
    0xd69906245565a910, 0xf40e35855771202a, 0x106aa07032bbd1b8,
    0x19a4c116b8d2d0c8, 0x1e376c085141ab53, 0x2748774cdf8eeb99,
    0x34b0bcb5e19b48a8, 0x391c0cb3c5c95a63, 0x4ed8aa4ae3418acb,
    0x5b9cca4f7763e373, 0x682e6ff3d6b2b8a3, 0x748f82ee5defb2fc,
    0x78a5636f43172f60, 0x84c87814a1f0ab72, 0x8cc702081a6439ec,
    0x90befffa23631e28, 0xa4506cebde82bde9, 0xbef9a3f7b2c67915,
    0xc67178f2e372532b, 0xca273eceea26619c, 0xd186b8c721c0c207,
    0xeada7dd6cde0eb1e, 0xf57d4f7fee6ed178, 0x06f067aa72176fba,
    0x0a637dc5a2c898a6, 0x113f9804bef90dae, 0x1b710b35131c471b,
    0x28db77f523047d84, 0x32caab7b40c72493, 0x3c9ebe0a15c9bebc,
    0x431d67c49c100d4c, 0x4cc5d4becb3e42b6, 0x597f299cfc657e2a,
    0x5fcb6fab3ad6faec, 0x6c44198c4a475817};
void writeWord(uint8_t *Out, uint64_t Value) {
  for (unsigned I = 0; I < 8; ++I)
    Out[I] = uint8_t(Value >> (56 - 8 * I));
}
} // namespace

SHA512::SHA512(bool Use384)
    : State(
          Use384
              ? std::array<uint64_t, 8>{0xcbbb9d5dc1059ed8, 0x629a292a367cd507,
                                        0x9159015a3070dd17, 0x152fecd8f70e5939,
                                        0x67332667ffc00b31, 0x8eb44a8768581511,
                                        0xdb0c2e0d64f98fa7, 0x47b5481dbefa4fa4}
              : std::array<uint64_t, 8>{0x6a09e667f3bcc908, 0xbb67ae8584caa73b,
                                        0x3c6ef372fe94f82b, 0xa54ff53a5f1d36f1,
                                        0x510e527fade682d1, 0x9b05688c2b3e6c1f,
                                        0x1f83d9abfb41bd6b,
                                        0x5be0cd19137e2179}),
      Use384(Use384) {}

void SHA512::block(const uint8_t *Data) {
  std::array<uint64_t, 80> W{};
  for (unsigned I = 0; I < 16; ++I)
    for (unsigned J = 0; J < 8; ++J)
      W[I] = (W[I] << 8) | Data[8 * I + J];
  for (unsigned I = 16; I < 80; ++I) {
    const auto X = W[I - 15], Y = W[I - 2];
    const auto S0 = std::rotr(X, 1) ^ std::rotr(X, 8) ^ (X >> 7);
    const auto S1 = std::rotr(Y, 19) ^ std::rotr(Y, 61) ^ (Y >> 6);
    W[I] = W[I - 16] + S0 + W[I - 7] + S1;
  }
  auto [A, B, C, D, E, F, G, H] = State;
  for (unsigned I = 0; I < 80; ++I) {
    const auto S0 = std::rotr(A, 28) ^ std::rotr(A, 34) ^ std::rotr(A, 39);
    const auto S1 = std::rotr(E, 14) ^ std::rotr(E, 18) ^ std::rotr(E, 41);
    const auto T1 = H + S1 + ((E & F) ^ (~E & G)) + RoundConstants[I] + W[I];
    const auto T2 = S0 + ((A & B) ^ (A & C) ^ (B & C));
    H = G;
    G = F;
    F = E;
    E = D + T1;
    D = C;
    C = B;
    B = A;
    A = T1 + T2;
  }
  const std::array<uint64_t, 8> Round{A, B, C, D, E, F, G, H};
  for (unsigned I = 0; I < 8; ++I)
    State[I] += Round[I];
}

void SHA512::update(std::string_view Data) {
  if (Data.size() > Limits::HardInputBytes - Bytes)
    throw Error("integrity_byte_budget_exceeded");
  Bytes += Data.size();
  while (!Data.empty()) {
    if (!Used && Data.size() >= Pending.size()) {
      block(reinterpret_cast<const uint8_t *>(Data.data()));
      Data.remove_prefix(Pending.size());
      continue;
    }
    const auto Count = std::min(Pending.size() - Used, Data.size());
    std::memcpy(Pending.data() + Used, Data.data(), Count);
    Used += Count;
    Data.remove_prefix(Count);
    if (Used == Pending.size()) {
      block(Pending.data());
      Used = 0;
    }
  }
}

std::string SHA512::result() const {
  auto Copy = *this;
  Copy.Pending[Copy.Used++] = 0x80;
  std::fill(Copy.Pending.begin() + Copy.Used, Copy.Pending.end(), 0);
  if (Copy.Used > 112) {
    Copy.block(Copy.Pending.data());
    Copy.Pending.fill(0);
  }
  // The 512 MiB admission bound leaves the high 64 length bits zero.
  writeWord(Copy.Pending.data() + 120, Bytes * 8);
  Copy.block(Copy.Pending.data());
  std::string Out(Use384 ? 48 : 64, '\0');
  for (size_t I = 0; I < Out.size() / 8; ++I)
    writeWord(reinterpret_cast<uint8_t *>(Out.data()) + 8 * I, Copy.State[I]);
  return Out;
}
} // namespace neverd::web
