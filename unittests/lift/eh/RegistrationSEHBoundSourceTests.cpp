//===- RegistrationSEHBoundSourceTests.cpp - Frame observer refusals -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

/// \file
/// Reject preserved SEH helpers that observe their physical caller frame.

#include "RegistrationSEHJumpNativeTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/X86/X86RegistrationSEHBoundCall.h"
#include "neverd/lift/X86/X86Regs.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/IR/Module.h"

#include <array>

namespace neverd::registration_test {

void checkSEHBoundSourceEdits(const llvm::Function &Parent,
                              const ExceptionFunction &Source,
                              const BinaryImage &Image) {
#ifdef LLVM_NEVERD_X86_REGISTRATION_EH
  va_t Helper = InvalidVA;
  for (const auto &Function : *Parent.getParent())
    if (Function.getName().starts_with("__nd_seh_bound_")) {
      auto Address = rewrite_source::getOriginalVA(Function);
      ASSERT_TRUE(bool(Address)) << llvm::toString(Address.takeError());
      ASSERT_TRUE(*Address);
      ASSERT_EQ(Helper, InvalidVA);
      Helper = **Address;
    }
  ASSERT_NE(Helper, InvalidVA);
  const auto *EH = Image.ExceptionMetadata.findFunction(Helper);
  ASSERT_TRUE(EH);
  checkSEHBoundCookieEdits(Parent, Source, *EH, Image);
  std::array<uint8_t, 8> Padding;
  Padding.fill(0x90);
  uint8_t *Window = nullptr;
  for (va_t Address = Helper; Address + Padding.size() <= EH->CodeRange.End;
       ++Address)
    if (const auto *Bytes = Image.readVA(Address, Padding.size());
        Bytes && std::equal(Padding.begin(), Padding.end(), Bytes)) {
      Window = const_cast<uint8_t *>(Bytes);
      break;
    }
  ASSERT_TRUE(Window);
  auto Restore = llvm::scope_exit(
      [&] { std::copy(Padding.begin(), Padding.end(), Window); });
  // Each same-size branch preserves the registration protocol. The source
  // solver must still complete, while native admission rejects dependence on
  // EBP, saved caller EBP, return PC, previous chain, or incoming EBX,
  // including a narrow alias. Both successors contain valid source
  // instructions.
  const std::array<std::array<uint8_t, 8>, 5> Mutations{{
      {{0x85, 0xed, 0x74, 0x02, 0x90, 0x90, 0x90, 0x90}},
      {{0x8b, 0x45, 0x00, 0x3c, 0x00, 0x74, 0x01, 0x90}},
      {{0x8b, 0x45, 0x04, 0x85, 0xc0, 0x74, 0x01, 0x90}},
      {{0x8b, 0x45, 0xf0, 0x85, 0xc0, 0x74, 0x01, 0x90}},
      {{0x85, 0xdb, 0x74, 0x02, 0x90, 0x90, 0x90, 0x90}},
  }};
  for (size_t Index = 0; Index != Mutations.size(); ++Index) {
    SCOPED_TRACE(Index);
    std::copy(Mutations[Index].begin(), Mutations[Index].end(), Window);
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Image));
    const auto Low =
        CFGBuilder().build(Image, Dec, Source.CodeRange.Begin, "observer");
    ASSERT_TRUE(Low.RegistrationStates);
    const auto &States = *Low.RegistrationStates;
    ASSERT_TRUE(States.Complete);
    EXPECT_TRUE(States.HasBoundSEHCallees);
    EXPECT_FALSE(States.SEHBoundCallsComplete);
    EXPECT_TRUE(States.SEHBoundCalls.empty());
    auto Error = validateCOFFRegistrationIR(Parent, Source, Image);
    ASSERT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
  }
  std::copy(Padding.begin(), Padding.end(), Window);
  if (Source.Personality != ExceptionPersonality::ExceptHandler4)
    return;
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Image));
  const auto Low =
      CFGBuilder().build(Image, Dec, Source.CodeRange.Begin, "cookie-borrow");
  ASSERT_TRUE(Low.RegistrationStates);
  ASSERT_TRUE(Low.RegistrationStates->SecurityCookiesComplete);
  auto Med = LowToMedConverter().convert(Low, Arch::X86, BinaryFormat::COFF);
  ASSERT_TRUE(getX86RegistrationSEHBoundCalls(Med));
  Med.RegistrationStates->SecurityCookiesComplete = false;
  EXPECT_FALSE(getX86RegistrationSEHBoundCalls(Med));
  Med.RegistrationStates->SecurityCookiesComplete = true;
  const va_t Cookie = Med.RegistrationStates->SecurityCookieVA;
  for (va_t Invalid : {va_t(0), va_t(UINT32_MAX - 2)}) {
    Med.RegistrationStates->SecurityCookieVA = Invalid;
    EXPECT_FALSE(getX86RegistrationSEHBoundCalls(Med));
  }
  Med.RegistrationStates->SecurityCookieVA = Cookie;

  // Retarget the actual LEA that supplies the borrowed scalar object. The
  // helper remains unchanged and still performs a checked four-byte store;
  // the caller must reject full and partial cookie overlap before lending it.
  std::optional<int32_t> Object;
  for (const auto &Call : Low.RegistrationStates->SEHBoundCalls)
    if (Call.TargetVA == Helper)
      for (const auto &Argument : Call.Arguments)
        if (Argument.ArgumentKind == RegistrationSEHBoundArgument::Kind::Object)
          Object = Argument.Offset;
  ASSERT_TRUE(Object);
  uint8_t *Displacement = nullptr;
  for (const auto &Block : Low.Blocks)
    for (const auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::INT_ADD && Op.NumInputs == 2 &&
          Op.Inputs[0].isReg() && Op.Inputs[0].Offset == x86reg::RBP &&
          Op.Inputs[1].isConst() && int32_t(Op.Inputs[1].Offset) == *Object) {
        auto *Bytes = const_cast<uint8_t *>(Image.readVA(Op.Addr, 3));
        if (Bytes && Bytes[0] == 0x8d && Bytes[1] == 0x45 &&
            int8_t(Bytes[2]) == *Object) {
          ASSERT_FALSE(Displacement);
          Displacement = Bytes + 2;
        }
      }
  ASSERT_TRUE(Displacement);
  const uint8_t Original = *Displacement;
  auto RestoreObject = llvm::scope_exit([&] { *Displacement = Original; });
  const auto &Chain = *Source.Registration;
  for (int32_t Slot : {Chain.EHCookieOffset, Chain.GSCookieOffset}) {
    if (Slot == -2)
      continue;
    for (int32_t Delta : {-1, 0, 1}) {
      const int64_t Offset =
          int64_t(*Chain.RegistrationOffset) + 16 + Slot + Delta;
      SCOPED_TRACE(Offset);
      ASSERT_GE(Offset, INT8_MIN);
      ASSERT_LE(Offset, INT8_MAX);
      *Displacement = uint8_t(Offset);
      const auto Changed = CFGBuilder().build(
          Image, Dec, Source.CodeRange.Begin, "changed-cookie-borrow");
      ASSERT_TRUE(Changed.RegistrationStates);
      EXPECT_FALSE(Changed.RegistrationStates->Complete);
      EXPECT_FALSE(Changed.RegistrationStates->SEHBoundCallsComplete);
      auto Error = validateCOFFRegistrationIR(Parent, Source, Image);
      ASSERT_TRUE(bool(Error));
      llvm::consumeError(std::move(Error));
    }
  }
#endif
}

} // namespace neverd::registration_test
