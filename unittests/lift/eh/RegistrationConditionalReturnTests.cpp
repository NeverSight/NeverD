//===- RegistrationConditionalReturnTests.cpp - Bound return proofs -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Returning cleanup and scalar values share the current invocation's inputs.
///
//===----------------------------------------------------------------------===//

#include "../../../lib/ir/low/X86/RegistrationABIPrivate.h"
#include "../../../lib/ir/low/X86/RegistrationConditionalFrame.h"
#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <utility>
#include <vector>

using namespace neverd;
using namespace neverd::registration_state;

namespace {

BinaryImage returnImage(va_t Base, const std::vector<uint8_t> &OtherReturn) {
  BinaryImage Image;
  Image.Arch = Arch::X86;
  Image.Bits = Bitness::Bits32;
  Image.Format = BinaryFormat::COFF;
  Image.Base = Base;
  Image.Entry = Base + 0x1000;
  Segment Text;
  Text.VA = Image.Entry;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  // cmp dword ptr [esp+4], 0; jnz other; mov eax, 37; ret;
  // other: mov eax, 91; <OtherReturn>
  Text.Data = {0x83, 0x7c, 0x24, 0x04, 0x00, 0x75, 0x06, 0xb8, 37,
               0x00, 0x00, 0x00, 0xc3, 0xb8, 91,   0x00, 0x00, 0x00};
  Text.Data.insert(Text.Data.end(), OtherReturn.begin(), OtherReturn.end());
  Text.Size = Text.FileSz = Text.Data.size();
  Section Code;
  Code.VA = Text.VA;
  Code.Size = Code.FileSz = Text.Size;
  Code.Flags = Text.Flags;
  Image.Sections.push_back(Code);
  Image.Segments.push_back(std::move(Text));
  return Image;
}

} // namespace

TEST(RegistrationConditionalReturn, BoundPathsDoNotBorrowAnIndependentABI) {
  for (va_t Base : {0x400000u, 0x18000000u})
    for (bool IndirectTail : {false, true}) {
      SCOPED_TRACE(Base);
      SCOPED_TRACE(IndirectTail);
      const auto Image =
          returnImage(Base, IndirectTail ? std::vector<uint8_t>{0xff, 0xe2}
                                         : std::vector<uint8_t>{0xc2, 4, 0});
      // The target has no uniform input-independent return contract.
      EXPECT_FALSE(getCheckedX86CalleeStackPop(Image, Image.Entry));
      LowOp Call;
      Call.Opcode = NdOp::CALL;
      Call.addInput(NdVar::cst(Image.Entry, 4));
      FrameState Parent;
      Parent.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
          FrameValue::frame(-64);
      const std::set<int32_t> Initialized{-64, -63, -62, -61};
      // Reverse and repeat inputs against one image. A target-only memo must
      // not lend a previous invocation's chosen return to another caller.
      for (unsigned Input : {0, 1, 2, 1, 0, 2})
        for (bool EffectRequested : {false, true}) {
          SCOPED_TRACE(Input);
          SCOPED_TRACE(EffectRequested);
          Parent.store(-64, 4,
                       Input == 2 ? FrameValue{} : FrameValue::constant(Input));
          size_t Work = 0;
          std::optional<FrameValue> Returned = FrameValue::constant(999);
          BoundPlainCalleeEffect Effect;
          Effect.StackPopBytes = 999;
          ConditionalCallResult Invocation{999, FrameValue::constant(999)};
          const bool Valid = registration_abi::hasConditionalCallFrame(
              Image, Call, Parent, Work, &Initialized, &Returned,
              EffectRequested ? &Effect : nullptr, nullptr, &Invocation);
          const bool Expected = Input == 0 || (Input == 1 && !IndirectTail);
          ASSERT_EQ(Valid, Expected);
          if (!Expected) {
            EXPECT_FALSE(Returned);
            EXPECT_FALSE(Invocation.StackPopBytes);
            EXPECT_FALSE(Invocation.ReturnValue);
            if (EffectRequested)
              EXPECT_EQ(Effect.StackPopBytes, 0u);
            continue;
          }
          EXPECT_EQ(Returned, FrameValue::constant(Input ? 91 : 37));
          EXPECT_EQ(Invocation.StackPopBytes, Input ? 4u : 0u);
          EXPECT_EQ(Invocation.ReturnValue, Returned);
          if (EffectRequested)
            EXPECT_EQ(Effect.StackPopBytes, Input ? 4u : 0u);
          ASSERT_GT(Work, 0u);
          ASSERT_LT(Work, limits::kMaxRegistrationEHStateWork);
          size_t Exact = limits::kMaxRegistrationEHStateWork - Work;
          EXPECT_TRUE(registration_abi::hasConditionalCallFrame(
              Image, Call, Parent, Exact, &Initialized, &Returned,
              EffectRequested ? &Effect : nullptr, nullptr, &Invocation));
          EXPECT_EQ(Exact, limits::kMaxRegistrationEHStateWork);
          EXPECT_EQ(Invocation.StackPopBytes, Input ? 4u : 0u);
          size_t Short = limits::kMaxRegistrationEHStateWork - Work + 1;
          EXPECT_FALSE(registration_abi::hasConditionalCallFrame(
              Image, Call, Parent, Short, &Initialized, &Returned,
              EffectRequested ? &Effect : nullptr, nullptr, &Invocation));
          EXPECT_FALSE(Returned);
          EXPECT_FALSE(Invocation.StackPopBytes);
          EXPECT_FALSE(Invocation.ReturnValue);
        }
      EXPECT_FALSE(getCheckedX86CalleeStackPop(Image, Image.Entry));
    }
}

TEST(RegistrationConditionalReturn, AllReachedReturnsMustRestoreTheFrame) {
  const std::vector<std::vector<uint8_t>> Returns{
      {0xc3},                   // Same cleanup, different scalar result.
      {0xcb},                   // A far return is not a near-call return.
      {0x83, 0xc4, 4, 0xc3},    // Unrestored ESP.
      {0xbb, 1, 0, 0, 0, 0xc3}, // Unrestored EBX.
      {0xc7, 4, 0x24, 0, 0, 0, 0, 0xc3}, // Overwritten caller PC.
      {0xeb, 0xfe}}; // A closed loop has no ordinary result.
  for (size_t Mode = 0; Mode != Returns.size(); ++Mode) {
    SCOPED_TRACE(Mode);
    const auto Image = returnImage(0x400000, Returns[Mode]);
    LowOp Call;
    Call.Opcode = NdOp::CALL;
    Call.addInput(NdVar::cst(Image.Entry, 4));
    FrameState Parent;
    Parent.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
        FrameValue::frame(-64);
    Parent.store(-64, 4, Mode == 0 ? FrameValue{} : FrameValue::constant(1));
    const std::set<int32_t> Initialized{-64, -63, -62, -61};
    size_t Work = 0;
    BoundPlainCalleeEffect Effect;
    std::optional<FrameValue> Returned = FrameValue::constant(999);
    ConditionalCallResult Invocation{999, FrameValue::constant(999)};
    const bool Valid = registration_abi::hasConditionalCallFrame(
        Image, Call, Parent, Work, &Initialized, &Returned, &Effect, nullptr,
        &Invocation);
    ASSERT_EQ(Valid, Mode == 0);
    if (Valid) {
      EXPECT_EQ(Effect.StackPopBytes, 0u);
      EXPECT_EQ(Returned, FrameValue{});
      EXPECT_EQ(Invocation.StackPopBytes, 0u);
      EXPECT_EQ(Invocation.ReturnValue, Returned);
    } else {
      EXPECT_FALSE(Returned);
      EXPECT_FALSE(Invocation.StackPopBytes);
      EXPECT_FALSE(Invocation.ReturnValue);
    }
  }
}
