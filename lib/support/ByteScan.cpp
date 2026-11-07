//===- ByteScan.cpp - Bounded vector text scanning
//-------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Runtime-dispatched ASCII run scanning with an exact scalar tail.
///
//===----------------------------------------------------------------------===//

#include "neverd/support/ByteScan.h"

#include "neverd/support/Limits.h"

#include <bit>

#if NEVERD_USE_HIGHWAY
#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "ByteScan.cpp"
#include "hwy/foreach_target.h"
#include "hwy/highway.h"

HWY_BEFORE_NAMESPACE();
namespace neverd::strings::detail {
namespace HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

size_t asciiPrefix(const uint8_t *Data, size_t Size) {
  const hn::ScalableTag<uint8_t> D;
  const size_t Width = hn::Lanes(D);
  size_t I = 0;
  for (; Size - I >= Width; I += Width) {
    const auto V = hn::LoadU(D, Data + I);
    const auto Printable =
        hn::And(hn::Ge(V, hn::Set(D, 0x20)), hn::Lt(V, hn::Set(D, 0x7f)));
    const auto Space = hn::Or(
        hn::Eq(V, hn::Set(D, '\t')),
        hn::Or(hn::Eq(V, hn::Set(D, '\n')), hn::Eq(V, hn::Set(D, '\r'))));
    const intptr_t Stop =
        hn::FindFirstTrue(D, hn::Not(hn::Or(Printable, Space)));
    if (Stop >= 0)
      return I + static_cast<size_t>(Stop);
  }
  while (I < Size && isTextASCII(Data[I]))
    ++I;
  return I;
}

/// The lanes of \p V that hold ASCII text (isTextASCII).
template <class D, class V> auto textLanes(D Tag, V Value) {
  using T = hn::TFromD<D>;
  const auto Printable = hn::And(hn::Ge(Value, hn::Set(Tag, T(0x20))),
                                 hn::Lt(Value, hn::Set(Tag, T(0x7f))));
  const auto Space = hn::Or(hn::Eq(Value, hn::Set(Tag, T('\t'))),
                            hn::Or(hn::Eq(Value, hn::Set(Tag, T('\n'))),
                                   hn::Eq(Value, hn::Set(Tag, T('\r')))));
  return hn::Or(Printable, Space);
}

/// The first NUL or control byte (isNulOrControl) of \p Data, or where the
/// whole vectors end.
size_t findNulOrControlLanes(const uint8_t *Data, size_t Size) {
  const hn::ScalableTag<uint8_t> D;
  const size_t Width = hn::Lanes(D);
  size_t I = 0;
  for (; Size - I >= Width; I += Width) {
    const auto V = hn::LoadU(D, Data + I);
    const auto Control =
        hn::AndNot(textLanes(D, V), hn::Lt(V, hn::Set(D, uint8_t(0x80))));
    const intptr_t At = hn::FindFirstTrue(D, Control);
    if (At >= 0)
      return I + static_cast<size_t>(At);
  }
  return I;
}

/// byteRunMasks over the whole 64-byte words of \p Count bytes: the words
/// it marks, none where a mask holds no 64 lanes' bits.
size_t byteRunMaskWords(const uint8_t *Data, size_t Count, bool Text,
                        uint64_t *Nul, uint64_t *Run) {
#if HWY_MAX_BYTES <= 64
  const hn::ScalableTag<uint8_t> D;
  const size_t Width = hn::Lanes(D);
  size_t W = 0;
  for (; Count - W * 64 >= 64; ++W) {
    uint64_t NulBits = 0, RunBits = 0;
    for (size_t I = 0; I < 64; I += Width) {
      const auto V = hn::LoadU(D, Data + W * 64 + I);
      const auto TextBytes = textLanes(D, V);
      const auto InRun =
          Text ? TextBytes
               : hn::Or(TextBytes, hn::Ge(V, hn::Set(D, uint8_t(0x80))));
      NulBits |= hn::BitsFromMask(D, hn::Eq(V, hn::Zero(D))) << I;
      RunBits |= hn::BitsFromMask(D, InRun) << I;
    }
    Nul[W] = NulBits;
    Run[W] = RunBits;
  }
  return W;
#else
  (void)Data;
  (void)Count;
  (void)Text;
  (void)Nul;
  (void)Run;
  return 0;
#endif
}

/// unitRunMasks over the whole 64-unit words of \p Count host-order units of
/// \p T, byte-swapped first with \p Swap: the words it marks.
template <typename T, bool Swap>
size_t unitRunMaskWords(const uint8_t *Data, size_t Count, uint64_t *Zero,
                        uint64_t *Run) {
#if HWY_MAX_BYTES <= 64
  const hn::ScalableTag<T> D;
  const hn::Repartition<uint8_t, decltype(D)> Bytes;
  const size_t Width = hn::Lanes(D);
  size_t W = 0;
  for (; Count - W * 64 >= 64; ++W) {
    uint64_t ZeroBits = 0, RunBits = 0;
    for (size_t I = 0; I < 64; I += Width) {
      auto V =
          hn::BitCast(D, hn::LoadU(Bytes, Data + (W * 64 + I) * sizeof(T)));
      if constexpr (Swap)
        V = hn::ReverseLaneBytes(V);
      auto NonText =
          hn::AndNot(textLanes(D, V), hn::Lt(V, hn::Set(D, T(0xa0))));
      if constexpr (sizeof(T) == 4)
        NonText = hn::Or(NonText, hn::Gt(V, hn::Set(D, T(0x10ffff))));
      ZeroBits |= hn::BitsFromMask(D, hn::Eq(V, hn::Zero(D))) << I;
      RunBits |= hn::BitsFromMask(D, hn::Not(NonText)) << I;
    }
    Zero[W] = ZeroBits;
    Run[W] = RunBits;
  }
  return W;
#else
  (void)Data;
  (void)Count;
  (void)Zero;
  (void)Run;
  return 0;
#endif
}
size_t unitRunMaskWords16(const uint8_t *Data, size_t Count, uint64_t *Zero,
                          uint64_t *Run) {
  return unitRunMaskWords<uint16_t, false>(Data, Count, Zero, Run);
}
size_t unitRunMaskWords16Swapped(const uint8_t *Data, size_t Count,
                                 uint64_t *Zero, uint64_t *Run) {
  return unitRunMaskWords<uint16_t, true>(Data, Count, Zero, Run);
}
size_t unitRunMaskWords32(const uint8_t *Data, size_t Count, uint64_t *Zero,
                          uint64_t *Run) {
  return unitRunMaskWords<uint32_t, false>(Data, Count, Zero, Run);
}
size_t unitRunMaskWords32Swapped(const uint8_t *Data, size_t Count,
                                 uint64_t *Zero, uint64_t *Run) {
  return unitRunMaskWords<uint32_t, true>(Data, Count, Zero, Run);
}
} // namespace HWY_NAMESPACE
} // namespace neverd::strings::detail
HWY_AFTER_NAMESPACE();
#endif

#if !NEVERD_USE_HIGHWAY || HWY_ONCE
namespace neverd::strings::detail {
#if NEVERD_USE_HIGHWAY
HWY_EXPORT(asciiPrefix);
HWY_EXPORT(findNulOrControlLanes);
HWY_EXPORT(byteRunMaskWords);
HWY_EXPORT(unitRunMaskWords16);
HWY_EXPORT(unitRunMaskWords16Swapped);
HWY_EXPORT(unitRunMaskWords32);
HWY_EXPORT(unitRunMaskWords32Swapped);
#endif

namespace {
/// Bytes or units a scan reads one at a time before it takes the vector
/// kernels: most gaps and runs in binary data are short.
constexpr size_t ScalarProbe = 16;

/// The first of Data's bytes from \p I that \p Stops, or Size.
template <typename Stop>
size_t scalarFrom(const uint8_t *Data, size_t I, size_t Size, Stop Stops) {
  while (I < Size && !Stops(Data[I]))
    ++I;
  return I;
}

/// Scalar, then vector from the probe on: the vector kernel stops at the
/// last whole vector, and the scalar tail finishes the span.
template <typename Stop, typename Kernel>
size_t findFirst(const uint8_t *Data, size_t Size, Stop Stops,
                 [[maybe_unused]] Kernel Vector) {
  const size_t Probe = Size < ScalarProbe ? Size : ScalarProbe;
  size_t I = scalarFrom(Data, 0, Probe, Stops);
  if (I < Probe || Probe == Size)
    return I;
#if NEVERD_USE_HIGHWAY
  I += Vector(Data + I, Size - I);
#endif
  return scalarFrom(Data, I, Size, Stops);
}
} // namespace

size_t findNulOrControl(const uint8_t *Data, size_t Size) {
  return findFirst(
      Data, Size, [](uint8_t B) { return isNulOrControl(B); },
      [](const uint8_t *At, size_t N) {
#if NEVERD_USE_HIGHWAY
        return HWY_DYNAMIC_DISPATCH(findNulOrControlLanes)(At, N);
#else
        (void)At;
        (void)N;
        return size_t{0};
#endif
      });
}

void byteRunMasks(const uint8_t *Data, size_t Count, bool Text, uint64_t *Nul,
                  uint64_t *Run) {
  size_t W = 0;
#if NEVERD_USE_HIGHWAY
  W = HWY_DYNAMIC_DISPATCH(byteRunMaskWords)(Data, Count, Text, Nul, Run);
#endif
  for (; W * 64 < Count; ++W) {
    const size_t Bytes = Count - W * 64 < 64 ? Count - W * 64 : 64;
    uint64_t NulBits = 0, RunBits = 0;
    for (size_t I = 0; I < Bytes; ++I) {
      const uint8_t B = Data[W * 64 + I];
      NulBits |= uint64_t(B == 0) << I;
      RunBits |= uint64_t(Text ? isTextASCII(B) : !isNulOrControl(B)) << I;
    }
    Nul[W] = NulBits;
    Run[W] = RunBits;
  }
}

void unitRunMasks(const uint8_t *Data, size_t Count, unsigned UnitBytes,
                  bool BigEndian, uint64_t *Zero, uint64_t *Run) {
  size_t W = 0;
#if NEVERD_USE_HIGHWAY
  // The kernels read host-order units, swapped where the order differs.
  const bool Swap = BigEndian != (std::endian::native == std::endian::big);
  if (UnitBytes == 2)
    W = Swap ? HWY_DYNAMIC_DISPATCH(unitRunMaskWords16Swapped)(Data, Count,
                                                               Zero, Run)
             : HWY_DYNAMIC_DISPATCH(unitRunMaskWords16)(Data, Count, Zero, Run);
  else
    W = Swap ? HWY_DYNAMIC_DISPATCH(unitRunMaskWords32Swapped)(Data, Count,
                                                               Zero, Run)
             : HWY_DYNAMIC_DISPATCH(unitRunMaskWords32)(Data, Count, Zero, Run);
#endif
  for (; W * 64 < Count; ++W) {
    const size_t Units = Count - W * 64 < 64 ? Count - W * 64 : 64;
    uint64_t ZeroBits = 0, RunBits = 0;
    for (size_t I = 0; I < Units; ++I) {
      const uint8_t *At = Data + (W * 64 + I) * UnitBytes;
      uint32_t Value = 0;
      for (unsigned B = 0; B < UnitBytes; ++B)
        Value |= uint32_t(At[B]) << (8 * (BigEndian ? UnitBytes - 1 - B : B));
      ZeroBits |= uint64_t(Value == 0) << I;
      RunBits |= uint64_t(!isNonTextUnit(Value)) << I;
    }
    Zero[W] = ZeroBits;
    Run[W] = RunBits;
  }
}

size_t longTextASCIIPrefix(const uint8_t *Data, size_t Size) {
#if NEVERD_USE_HIGHWAY
  if (Size >= ::neverd::limits::support::kByteScanLongTextASCIIPrefixSizeLimit)
    return HWY_DYNAMIC_DISPATCH(asciiPrefix)(Data, Size);
#endif
  size_t I = 0;
  while (I < Size && isTextASCII(Data[I]))
    ++I;
  return I;
}
} // namespace neverd::strings::detail
#endif
