//===- ByteScanTests.cpp - Bounded text acceleration contracts
//-------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Checks byte predicates, dispatch targets and inaccessible buffer tails.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/support/ByteScan.h"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#if NEVERD_TEST_HIGHWAY
#include "hwy/targets.h"
#endif
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

using neverd::strings::detail::byteRunMasks;
using neverd::strings::detail::findNulOrControl;
using neverd::strings::detail::isNonTextUnit;
using neverd::strings::detail::isNulOrControl;
using neverd::strings::detail::isTextASCII;
using neverd::strings::detail::textASCIIPrefix;
using neverd::strings::detail::unitRunMasks;

namespace {
/// A code unit of \p UnitBytes at \p At in the byte order \p BigEndian.
uint32_t unitAt(const uint8_t *At, unsigned UnitBytes, bool BigEndian) {
  uint32_t Value = 0;
  for (unsigned I = 0; I < UnitBytes; ++I)
    Value |= uint32_t(At[I]) << (8 * (BigEndian ? UnitBytes - 1 - I : I));
  return Value;
}

/// byteRunMasks and unitRunMasks over \p Count elements, as their contracts
/// mark each one: the terminators and the elements of a run.
template <typename Terminates, typename Runs>
void expectMasks(size_t Count, const std::vector<uint64_t> &Terminator,
                 const std::vector<uint64_t> &Run, Terminates IsTerminator,
                 Runs InRun) {
  ASSERT_EQ(Terminator.size(), (Count + 63) / 64);
  for (size_t I = 0; I < Terminator.size() * 64; ++I) {
    const bool Inside = I < Count;
    EXPECT_EQ(Terminator[I / 64] >> (I % 64) & 1,
              uint64_t(Inside && IsTerminator(I)))
        << I;
    EXPECT_EQ(Run[I / 64] >> (I % 64) & 1, uint64_t(Inside && InRun(I))) << I;
  }
}

void checkByteMasks(const uint8_t *Data, size_t Count) {
  for (const bool Text : {false, true}) {
    std::vector<uint64_t> Nul((Count + 63) / 64, ~uint64_t(0)),
        Run((Count + 63) / 64, ~uint64_t(0));
    byteRunMasks(Data, Count, Text, Nul.data(), Run.data());
    expectMasks(
        Count, Nul, Run, [&](size_t I) { return Data[I] == 0; },
        [&](size_t I) {
          return Text ? isTextASCII(Data[I]) : !isNulOrControl(Data[I]);
        });
  }
  size_t First = 0;
  while (First < Count && !isNulOrControl(Data[First]))
    ++First;
  EXPECT_EQ(findNulOrControl(Data, Count), First);
}

void checkUnitMasks(const uint8_t *Data, size_t Count, unsigned UnitBytes) {
  for (const bool BigEndian : {false, true}) {
    std::vector<uint64_t> Zero((Count + 63) / 64, ~uint64_t(0)),
        Run((Count + 63) / 64, ~uint64_t(0));
    unitRunMasks(Data, Count, UnitBytes, BigEndian, Zero.data(), Run.data());
    const auto unit = [&](size_t I) {
      return unitAt(Data + I * UnitBytes, UnitBytes, BigEndian);
    };
    expectMasks(
        Count, Zero, Run, [&](size_t I) { return unit(I) == 0; },
        [&](size_t I) { return !isNonTextUnit(unit(I)); });
  }
}

void checkRunMaskTargets() {
  // Every byte value at every lane, across two words.
  std::array<uint8_t, 192> Bytes;
  for (unsigned Byte = 0; Byte != 256; ++Byte)
    for (size_t Pos = 0; Pos != 130; ++Pos) {
      Bytes.fill('a');
      Bytes[Pos] = static_cast<uint8_t>(Byte);
      checkByteMasks(Bytes.data(), Bytes.size());
    }
  // Units either side of each class boundary, in both byte orders.
  const uint32_t Values[] = {
      0,      1,       9,        0x1f,     0x20,      0x7e,      0x7f,
      0x9f,   0xa0,    0xff,     0x100,    0x4e00,    0xd800,    0xdfff,
      0xffff, 0x10000, 0x10ffff, 0x110000, 0x1000000, 0xffffffff};
  std::array<uint8_t, 4 * 192> Units;
  for (const unsigned UnitBytes : {2u, 4u})
    for (const uint32_t Value : Values) {
      if (UnitBytes == 2 && Value > 0xffff)
        continue;
      for (size_t Pos = 0; Pos != 130; ++Pos)
        for (const bool BigEndian : {false, true}) {
          for (size_t I = 0; I != 192; ++I)
            for (unsigned B = 0; B < UnitBytes; ++B)
              Units[I * UnitBytes + B] =
                  B == (BigEndian ? UnitBytes - 1 : 0) ? 'a' : 0;
          for (unsigned B = 0; B < UnitBytes; ++B)
            Units[Pos * UnitBytes + B] = static_cast<uint8_t>(
                Value >> (8 * (BigEndian ? UnitBytes - 1 - B : B)));
          checkUnitMasks(Units.data(), 192, UnitBytes);
        }
    }
  // Every offset and count, over patterned bytes.
  for (size_t Offset = 0; Offset != 8; ++Offset)
    for (size_t Size = 0; Size != 300; ++Size) {
      for (size_t I = 0; I != Units.size(); ++I)
        Units[I] = static_cast<uint8_t>(
            (I * 37 + Size * 13) % 7 == 0 ? 0 : (I * 53 + Size) & 255);
      checkByteMasks(Units.data() + Offset, Size);
      checkUnitMasks(Units.data() + Offset, Size / 2, 2);
      checkUnitMasks(Units.data() + Offset, Size / 4, 4);
    }
}

size_t referencePrefix(const uint8_t *Data, size_t Size) {
  size_t I = 0;
  for (; I < Size; ++I)
    if (!(Data[I] >= 32 && Data[I] <= 126) && Data[I] != 9 && Data[I] != 10 &&
        Data[I] != 13)
      break;
  return I;
}

void checkPrefixTargets() {
  std::array<uint8_t, 384> Data;
  for (unsigned Byte = 0; Byte != 256; ++Byte) {
    for (size_t Pos = 0; Pos != 192; ++Pos) {
      Data.fill('a');
      Data[Pos] = Byte;
      EXPECT_EQ(textASCIIPrefix(Data.data(), Data.size()),
                referencePrefix(Data.data(), Data.size()))
          << Byte << ' ' << Pos;
    }
  }
  for (size_t Offset = 0; Offset != 64; ++Offset)
    for (size_t Size = 0; Size != 256; ++Size) {
      for (size_t I = 0; I != Data.size(); ++I)
        Data[I] = static_cast<uint8_t>((I * 37 + Size * 13) & 255);
      EXPECT_EQ(textASCIIPrefix(Data.data() + Offset, Size),
                referencePrefix(Data.data() + Offset, Size));
    }
}
} // namespace

TEST(ByteScan, EveryByteLaneAndAlignment) {
#if NEVERD_TEST_HIGHWAY
  const auto Targets = hwy::SupportedAndGeneratedTargets();
  struct Reset {
    ~Reset() { hwy::SetSupportedTargetsForTest(0); }
  } Restore;
  ASSERT_FALSE(Targets.empty());
  for (const auto Target : Targets) {
    SCOPED_TRACE(hwy::TargetName(Target));
    hwy::SetSupportedTargetsForTest(Target);
    checkPrefixTargets();
  }
#else
  checkPrefixTargets();
#endif
}

TEST(ByteScan, RunMasksMarkEveryByteAndUnit) {
#if NEVERD_TEST_HIGHWAY
  const auto Targets = hwy::SupportedAndGeneratedTargets();
  struct Reset {
    ~Reset() { hwy::SetSupportedTargetsForTest(0); }
  } Restore;
  ASSERT_FALSE(Targets.empty());
  for (const auto Target : Targets) {
    SCOPED_TRACE(hwy::TargetName(Target));
    hwy::SetSupportedTargetsForTest(Target);
    checkRunMaskTargets();
  }
#else
  checkRunMaskTargets();
#endif
}

TEST(ByteScan, EmptyAndGuardedTails) {
  EXPECT_EQ(textASCIIPrefix(nullptr, 0), 0u);
#ifdef _WIN32
  SYSTEM_INFO Info{};
  GetSystemInfo(&Info);
  const size_t Page = Info.dwPageSize;
  auto *Memory = static_cast<uint8_t *>(VirtualAlloc(
      nullptr, Page * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  ASSERT_NE(Memory, nullptr);
  DWORD Old = 0;
  ASSERT_TRUE(VirtualProtect(Memory + Page, Page, PAGE_NOACCESS, &Old));
#else
  const size_t Page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  auto *Memory =
      static_cast<uint8_t *>(mmap(nullptr, Page * 2, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANON, -1, 0));
  ASSERT_NE(Memory, MAP_FAILED);
  ASSERT_EQ(mprotect(Memory + Page, Page, PROT_NONE), 0);
#endif
  std::fill(Memory, Memory + Page, 'x');
#if NEVERD_TEST_HIGHWAY
  struct Reset {
    ~Reset() { hwy::SetSupportedTargetsForTest(0); }
  } Restore;
  for (const auto Target : hwy::SupportedAndGeneratedTargets()) {
    SCOPED_TRACE(hwy::TargetName(Target));
    hwy::SetSupportedTargetsForTest(Target);
#endif
    for (size_t Size = 0; Size <= 256; ++Size) {
      const auto *At = Memory + Page - Size;
      EXPECT_EQ(textASCIIPrefix(At, Size), Size);
      EXPECT_EQ(
          neverd::knownValidUTF8({reinterpret_cast<const char *>(At), Size}),
          static_cast<bool>(NEVERD_TEST_SIMDUTF));
      EXPECT_EQ(findNulOrControl(At, Size), Size);
      checkByteMasks(At, Size);
      for (const unsigned UnitBytes : {2u, 4u})
        checkUnitMasks(Memory + Page - Size / UnitBytes * UnitBytes,
                       Size / UnitBytes, UnitBytes);
    }
#if NEVERD_TEST_HIGHWAY
  }
#endif
#ifdef _WIN32
  EXPECT_TRUE(VirtualFree(Memory, 0, MEM_RELEASE));
#else
  EXPECT_EQ(munmap(Memory, Page * 2), 0);
#endif
}

TEST(ByteScan, UnicodeValidationDoesNotRepairOrNormalize) {
  const std::vector<std::string> Valid = {"",
                                          "ASCII",
                                          std::string("a\0b", 3),
                                          "\xc2\xa0",
                                          "\xe4\xb8\xad",
                                          "\xef\xbb\xbf",
                                          "\xef\xbf\xbf",
                                          "\xf4\x8f\xbf\xbf"};
  for (const auto &Value : Valid)
    for (size_t Prefix = 0; Prefix < 65; ++Prefix) {
      const auto Text = std::string(Prefix, 'x') + Value;
      EXPECT_EQ(neverd::knownValidUTF8(Text),
                static_cast<bool>(NEVERD_TEST_SIMDUTF));
    }
  const std::vector<std::string> Invalid = {"\x80",
                                            "\xc0\x80",
                                            "\xc1\xbf",
                                            "\xc2",
                                            "\xe0\x80\x80",
                                            "\xed\xa0\x80",
                                            "\xe1\x80",
                                            "\xf0\x80\x80\x80",
                                            "\xf4\x90\x80\x80",
                                            "\xf5\x80\x80\x80",
                                            "\xf1\x80\x80",
                                            "\xff"};
  for (const auto &Value : Invalid)
    for (size_t Prefix = 0; Prefix < 65; ++Prefix)
      EXPECT_FALSE(neverd::knownValidUTF8(std::string(Prefix, 'x') + Value));
  for (unsigned Byte = 0; Byte < 256; ++Byte) {
    const std::string Value(1, static_cast<char>(Byte));
    EXPECT_EQ(neverd::knownValidUTF8(Value), NEVERD_TEST_SIMDUTF && Byte < 128);
  }
}
