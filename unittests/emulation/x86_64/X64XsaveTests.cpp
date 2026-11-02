//===- X64XsaveTests.cpp - XSAVE format-specific initial SSE state -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "arch/x86_64/X64Machine.h"
#include "gtest/gtest.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Memory.h"

#include <algorithm>
#include <cstring>
#if defined(_M_X64)
#include <intrin.h>
#elif defined(__x86_64__)
#include <cpuid.h>
#endif

namespace neverd::emulation {
namespace {
#define NEVERD_XSAVE_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_XSAVE_TEXT(Name, Text) constexpr char Name[] = Text;
#define NEVERD_XSAVE_BYTES(Name, ...) constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64XsaveCases.def"
#undef NEVERD_XSAVE_BYTES
#undef NEVERD_XSAVE_TEXT
#undef NEVERD_XSAVE_VALUE
std::array<uint8_t, PacketBytes> packet(bool Compact, uint32_t Control) {
  std::array<uint8_t, PacketBytes> Bytes{};
  llvm::support::endian::write32le(Bytes.data() + ControlOffset, Control);
  llvm::support::endian::write64le(Bytes.data() + LayoutOffset,
                                   Compact ? CompactedLayout : 0);
  std::fill_n(Bytes.begin() + XmmOffset, XmmBytes, StaleByte);
  return Bytes;
}
TEST(X64Xsave, StandardInitSSEStillLoadsMXCSR) {
  const auto Bytes = packet(false, SeedControl);
  X64MachineState State;
  auto E = decodeX64XsaveState(State, Bytes);
  ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
  EXPECT_EQ(State.MXCSR, SeedControl);
  for (const auto &Lane : State.Xmm)
    EXPECT_EQ(Lane, RegisterValue({0, 0}));
}
TEST(X64Xsave, StandardInitSSERejectsReservedMXCSRWithoutPublication) {
  for (const bool Compact : {false, true}) {
    const auto Bytes = packet(Compact, InvalidControl);
    X64MachineState State;
    State.MXCSR = SeedControl;
    State.Xmm.front() = {SeedControl, SeedControl};
    const auto Before = State;
    auto E = decodeX64XsaveState(State, Bytes);
    if (Compact) {
      ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
      EXPECT_EQ(State.MXCSR, InitialControl);
      for (const auto &Lane : State.Xmm)
        EXPECT_EQ(Lane, RegisterValue({0, 0}));
    } else {
      EXPECT_TRUE(bool(E));
      llvm::consumeError(std::move(E));
      EXPECT_EQ(State, Before);
    }
  }
}
#if defined(__x86_64__) || defined(_M_X64)
std::array<unsigned, 4> cpuid(unsigned Leaf, unsigned Subleaf = 0) {
#ifdef _MSC_VER
  int Values[4];
  __cpuidex(Values, Leaf, Subleaf);
  return {unsigned(Values[0]), unsigned(Values[1]), unsigned(Values[2]),
          unsigned(Values[3])};
#else
  std::array<unsigned, 4> Values{};
  __cpuid_count(Leaf, Subleaf, Values[0], Values[1], Values[2], Values[3]);
  return Values;
#endif
}
#endif
struct ExtendedLayout {
  uint64_t Mask;
  size_t Bytes;
  size_t PaddingBegin;
  size_t PaddingBytes;
};
constexpr ExtendedLayout ExtendedLayouts[] = {
#define NEVERD_XSAVE_LAYOUT(Mask, Bytes, PaddingBegin, PaddingBytes)           \
  {Mask, Bytes, PaddingBegin, PaddingBytes},
#include "X64XsaveCases.def"
#undef NEVERD_XSAVE_LAYOUT
};
std::vector<uint8_t> extendedPacket(const ExtendedLayout &Layout,
                                    const X64MachineState &State) {
  std::vector<uint8_t> Bytes(Layout.Bytes);
  llvm::cantFail(encodeX64XsaveState(State, Bytes, true));
  llvm::support::endian::write64le(Bytes.data() + PresentOffset,
                                   FPAndSSEMask | Layout.Mask);
  llvm::support::endian::write64le(Bytes.data() + LayoutOffset,
                                   CompactedLayout | Layout.Mask);
  return Bytes;
}
TEST(X64Xsave, CompactedInitialExtensionsPreserveTheCompleteFPState) {
  X64MachineState Before;
  Before.MXCSR = SeedControl;
  Before.Xmm.front() = {SeedControl, InvalidControl};
  for (const auto &Layout : ExtendedLayouts) {
    SCOPED_TRACE(Layout.Mask);
    const auto Bytes = extendedPacket(Layout, Before);
    auto State = Before;
    auto E = decodeX64XsaveState(State, Bytes);
    ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
    EXPECT_EQ(State, Before);
  }
}
TEST(X64Xsave, CompactedOffsetsFollowLayoutRatherThanPresentBits) {
  X64MachineState Before;
  Before.MXCSR = SeedControl;
  Before.Xmm.front() = {SeedControl, InvalidControl};
  std::vector<uint8_t> Bytes(NativeBytes);
  llvm::cantFail(encodeX64XsaveState(Before, Bytes, true));
  llvm::support::endian::write64le(Bytes.data() + LayoutOffset, NativeLayout);
  for (const auto Entry :
       {std::pair{NativePresent, NativeCETUserOffset},
        std::pair{NativeSupervisorPresent, NativeCETSupervisorOffset}}) {
    llvm::support::endian::write64le(Bytes.data() + PresentOffset, Entry.first);
    std::fill(Bytes.begin() + PacketBytes, Bytes.begin() + Entry.second,
              StaleByte);
    auto State = Before;
    auto E = decodeX64XsaveState(State, Bytes);
    ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
    EXPECT_EQ(State, Before);
  }
}
TEST(X64Xsave, WideLayoutIgnoresAbsentComponentsAndAlignmentPadding) {
  X64MachineState Before;
  Before.MXCSR = SeedControl;
  Before.Xmm.back() = {SeedControl, InvalidControl};
  auto Bytes =
      extendedPacket({WideLayout & ~CompactedLayout, WideBytes, 0, 0}, Before);
  llvm::support::endian::write64le(Bytes.data() + PresentOffset, NativePresent);
  std::fill(Bytes.begin() + PacketBytes, Bytes.end(), StaleByte);
  std::fill(Bytes.begin() + WideCETUserOffset, Bytes.begin() + WideCETEnd, 0);
  auto State = Before;
  auto E = decodeX64XsaveState(State, Bytes);
  ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
  EXPECT_EQ(State, Before);
  for (const auto &Layout : ExtendedLayouts) {
    if (!Layout.PaddingBytes)
      continue;
    auto Padded = extendedPacket(Layout, Before);
    std::fill_n(Padded.begin() + Layout.PaddingBegin, Layout.PaddingBytes,
                StaleByte);
    E = decodeX64XsaveState(State, Padded);
    ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
    EXPECT_EQ(State, Before);
  }
}
TEST(X64Xsave, EveryNonInitialExtensionByteRejectsWithoutPublication) {
  X64MachineState Before;
  Before.MXCSR = SeedControl;
  Before.Xmm.front() = {SeedControl, InvalidControl};
  for (const auto &Layout : ExtendedLayouts) {
    SCOPED_TRACE(Layout.Mask);
    auto Bytes = extendedPacket(Layout, Before);
    for (size_t I = PacketBytes; I < Bytes.size(); ++I) {
      if (I >= Layout.PaddingBegin &&
          I - Layout.PaddingBegin < Layout.PaddingBytes)
        continue;
      SCOPED_TRACE(I);
      Bytes[I] = StaleByte;
      auto State = Before;
      auto E = decodeX64XsaveState(State, Bytes);
      EXPECT_TRUE(bool(E));
      llvm::consumeError(std::move(E));
      EXPECT_EQ(State, Before);
      Bytes[I] = 0;
    }
  }
}
TEST(X64Xsave, TruncatedAndUnknownExtensionsCannotPublishState) {
  X64MachineState Before;
  Before.MXCSR = SeedControl;
  for (const auto &Layout : ExtendedLayouts) {
    auto Bytes = extendedPacket(Layout, Before);
    for (size_t Size = PacketBytes; Size < Bytes.size(); ++Size) {
      auto State = Before;
      auto E =
          decodeX64XsaveState(State, llvm::ArrayRef(Bytes).take_front(Size));
      EXPECT_TRUE(bool(E));
      llvm::consumeError(std::move(E));
      EXPECT_EQ(State, Before);
    }
    for (const uint64_t Header :
         {uint64_t(0), CompactedLayout | Layout.Mask | UnsupportedComponent}) {
      llvm::support::endian::write64le(Bytes.data() + LayoutOffset, Header);
      auto State = Before;
      auto E = decodeX64XsaveState(State, Bytes);
      EXPECT_TRUE(bool(E));
      llvm::consumeError(std::move(E));
      EXPECT_EQ(State, Before);
    }
    // An undeclared packing/alignment must not hide live state in a tail that
    // the known compacted profile does not own.
    llvm::support::endian::write64le(Bytes.data() + LayoutOffset,
                                     CompactedLayout | Layout.Mask);
    Bytes.resize(Layout.Bytes + UnexpectedPaddingBytes);
    Bytes.back() = StaleByte;
    auto State = Before;
    auto E = decodeX64XsaveState(State, Bytes);
    EXPECT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
    EXPECT_EQ(State, Before);
  }
}
TEST(X64Xsave, AbsentExtensionsIgnoreStaleBytes) {
  X64MachineState Before;
  Before.MXCSR = SeedControl;
  for (const auto &Layout : ExtendedLayouts) {
    auto Bytes = extendedPacket(Layout, Before);
    llvm::support::endian::write64le(Bytes.data() + PresentOffset,
                                     FPAndSSEMask);
    std::fill(Bytes.begin() + PacketBytes, Bytes.end(), StaleByte);
    auto State = Before;
    auto E = decodeX64XsaveState(State, Bytes);
    ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
    EXPECT_EQ(State, Before);
  }
}
class X64XsaveHost : public testing::TestWithParam<bool> {};
TEST_P(X64XsaveHost, ActualRestoreDistinguishesStandardAndCompactedMXCSR) {
#if defined(__x86_64__) || defined(_M_X64)
  const bool Compact = GetParam();
  if (cpuid(0)[0] < XsaveLeaf ||
      (cpuid(ProcessorLeaf)[2] & XsaveAndOSXsave) != XsaveAndOSXsave ||
      (Compact && !(cpuid(XsaveLeaf, XsaveSubleaf)[0] & XsaveCompaction)))
    GTEST_SKIP() << FeatureUnavailable;
  alignas(Alignment) auto Input = packet(Compact, SeedControl);
  alignas(Alignment) std::array<uint8_t, FXBytes> Output{}, Host{};
#ifdef _WIN32
  const auto Code = llvm::ArrayRef<uint8_t>(Win64Restore);
#else
  const auto Code = llvm::ArrayRef<uint8_t>(SysVRestore);
#endif
  std::error_code EC;
  auto Block = llvm::sys::Memory::allocateMappedMemory(
      Code.size(), nullptr,
      llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_WRITE, EC);
  ASSERT_FALSE(bool(EC)) << EC.message();
  auto Release = llvm::scope_exit(
      [&] { (void)llvm::sys::Memory::releaseMappedMemory(Block); });
  std::memcpy(Block.base(), Code.data(), Code.size());
  EC = llvm::sys::Memory::protectMappedMemory(
      Block, llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_EXEC);
  ASSERT_FALSE(bool(EC)) << EC.message();
  llvm::sys::Memory::InvalidateInstructionCache(Block.base(), Code.size());
  auto Restore =
      reinterpret_cast<void (*)(void *, void *, void *)>(Block.base());
  Restore(Input.data(), Output.data(), Host.data());
  const auto ActualControl =
      llvm::support::endian::read32le(Output.data() + ControlOffset);
  EXPECT_EQ(ActualControl, Compact ? InitialControl : SeedControl);
  EXPECT_TRUE(std::all_of(Output.begin() + XmmOffset,
                          Output.begin() + XmmOffset + XmmBytes,
                          [](uint8_t Byte) { return Byte == 0; }));
  X64MachineState State;
  auto E = decodeX64XsaveState(State, Input);
  ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
  EXPECT_EQ(State.MXCSR, ActualControl);
#else
  GTEST_SKIP() << HostUnavailable;
#endif
}
INSTANTIATE_TEST_SUITE_P(Formats, X64XsaveHost, testing::Values(false, true));
} // namespace
} // namespace neverd::emulation
