//===- X86SHA512Reference.h - Independent scalar SHA512 oracle -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Scalar SHA512 equations for lifting and executable source regression tests.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LIFT_X86_64_X86SHA512REFERENCE_H
#define NEVERD_UNITTESTS_LIFT_X86_64_X86SHA512REFERENCE_H

#include <array>
#include <bit>
#include <cstdint>

namespace neverd::test {
using Words = std::array<uint64_t, 8>;

inline std::array<uint64_t, 4> sha512Reference(unsigned Opcode, const Words &D,
                                               const Words &S, const Words &W) {
  std::array<uint64_t, 4> R;
  if (Opcode == 0xcc) {
    for (unsigned I = 0; I != 4; ++I) {
      const auto V = I == 3 ? S[0] : D[I + 1];
      R[I] = D[I] + (std::rotr(V, 1) ^ std::rotr(V, 8) ^ (V >> 7));
    }
  } else if (Opcode == 0xcd) {
    for (unsigned I = 0; I != 4; ++I) {
      const auto V = I < 2 ? S[I + 2] : R[I - 2];
      R[I] = D[I] + (std::rotr(V, 19) ^ std::rotr(V, 61) ^ (V >> 6));
    }
  } else {
    uint64_t A = S[3], B = S[2], C = D[3], Dd = D[2];
    uint64_t E = S[1], F = S[0], G = D[1], H = D[0];
    for (unsigned I = 0; I != 2; ++I) {
      const auto T1 = H +
                      (std::rotr(E, 14) ^ std::rotr(E, 18) ^ std::rotr(E, 41)) +
                      ((E & F) ^ (~E & G)) + W[I];
      const auto T2 = (std::rotr(A, 28) ^ std::rotr(A, 34) ^ std::rotr(A, 39)) +
                      ((A & B) ^ (A & C) ^ (B & C));
      H = G;
      G = F;
      F = E;
      E = Dd + T1;
      Dd = C;
      C = B;
      B = A;
      A = T1 + T2;
    }
    R = {F, E, B, A};
  }
  return R;
}

} // namespace neverd::test

#endif // NEVERD_UNITTESTS_LIFT_X86_64_X86SHA512REFERENCE_H
