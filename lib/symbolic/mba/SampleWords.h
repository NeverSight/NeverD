//===- SampleWords.h - Fixed verification assignments ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Compile-time prefix of the existing deterministic native sample stream.
/// The standard engine's parameters define every word; longer checks use the
/// ordinary engine from its original seed.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SYMBOLIC_MBA_SAMPLEWORDS_H
#define NEVERD_SYMBOLIC_MBA_SAMPLEWORDS_H

#include "Limits.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>

namespace neverd::symbolic::detail {

inline constexpr auto kVerificationSampleWords = [] {
  using Engine = std::mt19937_64;
  static_assert(Engine::word_size == 64);
  std::array<uint64_t, Engine::state_size> State{};
  std::array<uint64_t, limits::kVerificationCachedWords> Words{};
  State[0] = limits::kVerificationSampleSeed;
  for (size_t I = 1; I < State.size(); ++I)
    State[I] = Engine::initialization_multiplier *
                   (State[I - 1] ^ (State[I - 1] >> (Engine::word_size - 2))) +
               I;
  constexpr uint64_t Low = (uint64_t(1) << Engine::mask_bits) - 1;
  for (size_t K = 0; K < Words.size(); ++K) {
    const size_t I = K % Engine::state_size;
    const uint64_t X =
        (State[I] & ~Low) | (State[(I + 1) % Engine::state_size] & Low);
    uint64_t Y = State[I] =
        State[(I + Engine::shift_size) % Engine::state_size] ^ (X >> 1) ^
        ((X & 1) ? Engine::xor_mask : 0);
    Y ^= (Y >> Engine::tempering_u) & Engine::tempering_d;
    Y ^= (Y << Engine::tempering_s) & Engine::tempering_b;
    Y ^= (Y << Engine::tempering_t) & Engine::tempering_c;
    Words[K] = Y ^ (Y >> Engine::tempering_l);
  }
  return Words;
}();

} // namespace neverd::symbolic::detail

#endif // NEVERD_SYMBOLIC_MBA_SAMPLEWORDS_H
