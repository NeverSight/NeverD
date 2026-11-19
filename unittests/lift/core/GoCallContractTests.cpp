//===- GoCallContractTests.cpp - Registers a call from Go code keeps ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Go code has no callee-saved registers: a value a Go caller left in RBX is
/// gone after a call, where System V would keep it, and RBX is where Go
/// returns a call's second result.  The runtime's write barriers keep every
/// general-purpose register but the one they return buffer space in.
//===----------------------------------------------------------------------===//

#include "../language/GoRuntimeEHTestsDetail.h"
#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/CallRegisterEffects.h"
#include "neverd/ir/low/GoCallContract.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedCallLeftovers.h"
#include "neverd/lift/X86/X86Regs.h"

#include <map>
#include <string>
#include <vector>

namespace {
using namespace neverd;
using namespace neverd::go_eh_test;

constexpr va_t IndirectCaller = kTextVA + 0x100;
constexpr va_t BarrierCaller = kTextVA + 0x180;
constexpr va_t Barrier = kTextVA + 0x200;
constexpr va_t CFunction = kTextVA + 0x300;

GoFuncSpec goFunction(const std::string &Name, va_t Entry) {
  GoFuncSpec F;
  F.Name = Name;
  F.EntryVA = Entry;
  F.PcSP.Steps = {{0, 0x40}};
  return F;
}

/// `mov ebx, 5; call rsi; lea rax, [rbx + 1]; ret` in main.indirect and in
/// the C function x_cgo_init, whose record has no pc-SP table; the same
/// around a call to runtime.gcWriteBarrier2 in main.barrier.  Without the
/// pclntab every function keeps the System V contract.
BinaryImage image(bool GoImage,
                  const std::string &BarrierName = "runtime.gcWriteBarrier2") {
  GoTestImage T;
  const std::vector<uint8_t> Indirect = {0xBB, 5,    0,    0,    0,    0xFF,
                                         0xD6, 0x48, 0x8D, 0x43, 0x01, 0xC3};
  T.writeText(IndirectCaller, Indirect);
  T.writeText(CFunction, Indirect);
  std::vector<uint8_t> ViaBarrier = {0xBB, 5, 0, 0, 0};
  const std::vector<uint8_t> Call = makeCall(BarrierCaller + 5, Barrier);
  ViaBarrier.insert(ViaBarrier.end(), Call.begin(), Call.end());
  ViaBarrier.insert(ViaBarrier.end(), {0x48, 0x8D, 0x43, 0x01, 0xC3});
  T.writeText(BarrierCaller, ViaBarrier);
  // `mov r11d, 16; ret`: the stub's own work; its contract is Go's.
  T.writeText(Barrier, {0x41, 0xBB, 0x10, 0, 0, 0, 0xC3});
  if (GoImage) {
    GoFuncSpec C;
    C.Name = "x_cgo_init";
    C.EntryVA = CFunction;
    T.installPclnTab(buildPclnTab(kGo116Magic,
                                  {goFunction("main.indirect", IndirectCaller),
                                   goFunction("main.barrier", BarrierCaller),
                                   goFunction(BarrierName, Barrier), C},
                                  kTextVA + 0x400)
                         .Bytes);
    go_loader::parseGoExceptions(T.Img);
  }
  return std::move(T.Img);
}

LowFunc build(const BinaryImage &Img, va_t Entry) {
  Decoder Dec;
  EXPECT_TRUE(Dec.init(Img));
  return CFGBuilder().build(Img, Dec, Entry);
}

MedFunc lift(const BinaryImage &Img, va_t Entry) {
  LowToMedConverter Converter;
  Converter.setBinaryImage(&Img);
  return Converter.convert(build(Img, Entry), Img.Arch, Img.Format);
}

const MedOp *onlyCall(const MedFunc &F) {
  const MedOp *Found = nullptr;
  for (const MedBlock &Block : F.Blocks)
    for (const MedOp &Op : Block.Ops)
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
        EXPECT_EQ(Found, nullptr);
        Found = &Op;
      }
  return Found;
}

/// Whether the function's `lea rax, [rbx + 1]` reads the value the call left
/// in RBX rather than the 5 the function stored there before the call.
bool readsRBXTheCallLeft(const MedFunc &F, const MedOp &Call) {
  for (const MedBlock &Block : F.Blocks)
    for (const MedOp &Op : Block.Ops)
      if (Op.Addr > Call.Addr && Op.NumInputs &&
          Op.Inputs[0].Kind == MedVar::Reg &&
          Op.Inputs[0].RegOff == x86reg::RBX)
        for (const MedCallClobber &Clobber : F.CallClobbers)
          if (Clobber.CallSiteId == Call.CallSiteId &&
              Clobber.Value.Id == Op.Inputs[0].Id &&
              Clobber.Value.SSAVer == Op.Inputs[0].SSAVer)
            return true;
  return false;
}

TEST(GoCallContract, ACallFromGoCodeChangesRBX) {
  const BinaryImage Img = image(true);
  ASSERT_TRUE(isGoCode(Img, IndirectCaller));
  const MedFunc F = lift(Img, IndirectCaller);
  const MedOp *Call = onlyCall(F);
  ASSERT_NE(Call, nullptr);
  EXPECT_EQ(Call->RegisterContract, CallRegisterContract::GoCode);
  const TargetRegInfo &TRI = getTargetRegInfo(Arch::X64);
  for (uint64_t Register :
       {x86reg::RBX, x86reg::R12, x86reg::R13, x86reg::R14, x86reg::R15})
    EXPECT_TRUE(callClobbersRegister(*Call, Register, 8, TRI, Img.Format))
        << Register;
  EXPECT_FALSE(callClobbersRegister(*Call, x86reg::RBP, 8, TRI, Img.Format));
  EXPECT_TRUE(readsRBXTheCallLeft(F, *Call));
}

TEST(GoCallContract, SystemVCodeKeepsRBX) {
  const BinaryImage Img = image(false);
  EXPECT_FALSE(isGoCode(Img, IndirectCaller));
  const MedFunc F = lift(Img, IndirectCaller);
  const MedOp *Call = onlyCall(F);
  ASSERT_NE(Call, nullptr);
  EXPECT_EQ(Call->RegisterContract, CallRegisterContract::Platform);
  EXPECT_FALSE(callClobbersRegister(*Call, x86reg::RBX, 8,
                                    getTargetRegInfo(Arch::X64), Img.Format));
  EXPECT_FALSE(readsRBXTheCallLeft(F, *Call));
}

TEST(GoCallContract, CCodeACgoImageLinksKeepsThePlatformContract) {
  const BinaryImage Img = image(true);
  EXPECT_FALSE(isGoCode(Img, CFunction));
  const MedOp *Call = onlyCall(lift(Img, CFunction));
  ASSERT_NE(Call, nullptr);
  EXPECT_EQ(Call->RegisterContract, CallRegisterContract::Platform);
}

TEST(GoCallContract, AWriteBarrierKeepsEveryRegisterButItsBuffer) {
  const BinaryImage Img = image(true);
  ASSERT_NE(goWriteBarrierAt(Img, Barrier), nullptr);
  const MedFunc F = lift(Img, BarrierCaller);
  const MedOp *Call = onlyCall(F);
  ASSERT_NE(Call, nullptr);
  EXPECT_EQ(Call->RegisterContract, CallRegisterContract::GoWriteBarrier);
  // The call defines the buffer pointer it returns.
  EXPECT_EQ(Call->Output.Kind, MedVar::Reg);
  EXPECT_EQ(Call->Output.RegOff, x86reg::R11);
  const TargetRegInfo &TRI = getTargetRegInfo(Arch::X64);
  for (uint64_t Register :
       {x86reg::RAX, x86reg::RBX, x86reg::RCX, x86reg::RDI, x86reg::R8,
        x86reg::R12, x86reg::R13, x86reg::R14, x86reg::R15})
    EXPECT_FALSE(callClobbersRegister(*Call, Register, 8, TRI, Img.Format))
        << Register;
  EXPECT_TRUE(callClobbersRegister(*Call, x86reg::XMM0, 16, TRI, Img.Format));
  EXPECT_FALSE(readsRBXTheCallLeft(F, *Call));
}

TEST(GoCallContract, AStoringWriteBarrierReturnsNothing) {
  // Go 1.11-1.20 `runtime.gcWriteBarrier` performs the store itself: the
  // call changes no general-purpose register, RAX included.
  const BinaryImage Img = image(true, "runtime.gcWriteBarrier");
  ASSERT_NE(goWriteBarrierAt(Img, Barrier), nullptr);
  const MedFunc F = lift(Img, BarrierCaller);
  const MedOp *Call = onlyCall(F);
  ASSERT_NE(Call, nullptr);
  EXPECT_EQ(Call->RegisterContract, CallRegisterContract::GoWriteBarrier);
  EXPECT_EQ(Call->Output.Size, 0u);
  const TargetRegInfo &TRI = getTargetRegInfo(Arch::X64);
  for (uint64_t Register : {x86reg::RAX, x86reg::RBX, x86reg::RDI, x86reg::R11})
    EXPECT_FALSE(callClobbersRegister(*Call, Register, 8, TRI, Img.Format))
        << Register;
  EXPECT_FALSE(readsRBXTheCallLeft(F, *Call));
}

TEST(GoCallContract, SummariesFollowTheContract) {
  const BinaryImage Img = image(true);
  const GPRFamilyMask RBX = GPRFamilyMask(1) << (x86reg::RBX / 8);
  const GPRFamilyMask RBP = GPRFamilyMask(1) << (x86reg::RBP / 8);
  const GPRFamilyMask R11 = GPRFamilyMask(1) << (x86reg::R11 / 8);

  const LocalRegisterEffect Caller = localRegisterEffect(
      Img, build(Img, IndirectCaller), nullptr, nullptr, nullptr, nullptr);
  ASSERT_TRUE(Caller.CallClobbers.has_value());
  EXPECT_TRUE(*Caller.CallClobbers & RBX);
  EXPECT_FALSE(*Caller.CallClobbers & RBP);

  // The barrier's body would read and write every register it saves; its
  // documented contract writes R11 alone of them and reads only g.
  const LocalRegisterEffect Effect = localRegisterEffect(
      Img, build(Img, Barrier), nullptr, nullptr, nullptr, nullptr);
  EXPECT_TRUE(Effect.Writes & R11);
  EXPECT_FALSE(Effect.Writes & RBX);
  EXPECT_FALSE(Effect.Unknown);

  const LocalRegisterEffect Platform = localRegisterEffect(
      Img, build(Img, CFunction), nullptr, nullptr, nullptr, nullptr);
  EXPECT_FALSE(Platform.CallClobbers.has_value());
}

//===----------------------------------------------------------------------===//
// Register arguments
//===----------------------------------------------------------------------===//

constexpr va_t GoCallee = kTextVA + 0x100;
constexpr va_t GoCaller = kTextVA + 0x200;
constexpr va_t Morestack = kTextVA + 0x300;
constexpr va_t Assembly = kTextVA + 0x400;

/// A Go 1.18+ image whose main.f takes two register arguments and grows its
/// stack the way the compiler emits it, and whose main.g passes them:
///
///   main.f: cmp rsp, [r14+0x10]; jbe grow; lea rax, [rax+rbx]; ret
///     grow: mov [rsp+8], rax; mov [rsp+0x10], rbx; call morestack_noctxt
///           mov rax, [rsp+8]; mov rbx, [rsp+0x10]; jmp main.f
///   main.g: mov eax, 1; mov ebx, 2; call main.f; ret
BinaryImage registerArgumentImage() {
  GoTestImage T;
  T.writeText(GoCallee,
              {0x49, 0x3B, 0x66, 0x10, 0x76, 0x05, 0x48, 0x8D, 0x04, 0x18, 0xC3,
               0x48, 0x89, 0x44, 0x24, 0x08, 0x48, 0x89, 0x5C, 0x24, 0x10});
  T.writeText(GoCallee + 0x15, makeCall(GoCallee + 0x15, Morestack));
  T.writeText(GoCallee + 0x1A, {0x48, 0x8B, 0x44, 0x24, 0x08, 0x48, 0x8B, 0x5C,
                                0x24, 0x10, 0xEB, 0xDA});
  T.writeText(GoCaller, {0xB8, 1, 0, 0, 0, 0xBB, 2, 0, 0, 0});
  T.writeText(GoCaller + 0x0A, makeCall(GoCaller + 0x0A, GoCallee));
  T.writeText(GoCaller + 0x0F, {0xC3});
  T.writeText(Morestack, {0xC3});
  T.writeText(Assembly, {0x48, 0x8D, 0x04, 0x18, 0xC3});
  GoModuleInfo Module;
  Module.PclnTabMagic = 0xFFFFFFF1u;
  Module.RegisterArguments = true;
  Module.GoCode = {{GoCallee, kTextVA + 0x500}};
  Module.GoAssembly = {{Assembly, kTextVA + 0x500}};
  Module.MorestackEntries = {Morestack};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  return std::move(T.Img);
}

TEST(GoRegisterArguments, TheStackGrowthPathSpillsExactlyTheArguments) {
  const BinaryImage Img = registerArgumentImage();
  const auto Spills = goMorestackSpills(Img, build(Img, GoCallee).Blocks);
  ASSERT_TRUE(Spills.has_value());
  ASSERT_EQ(Spills->size(), 2u);
  EXPECT_EQ((*Spills)[0].Register, x86reg::RAX);
  EXPECT_EQ((*Spills)[0].EntryOffset, 8);
  EXPECT_EQ((*Spills)[1].Register, x86reg::RBX);
  EXPECT_EQ((*Spills)[1].EntryOffset, 0x10);
  // A caller without a stack-growth path has no such list.
  EXPECT_FALSE(goMorestackSpills(Img, build(Img, GoCaller).Blocks));
}

TEST(GoRegisterArguments, AWideningReloadRestoresASpilledByte) {
  // main.h(b bool): cmp rsp, [r14+0x10]; jbe grow; mov eax, ecx; ret
  //   grow: mov [rsp+8], cl; call morestack_noctxt
  //         movzx ecx, byte [rsp+8]; jmp main.h
  GoTestImage T;
  constexpr va_t Byte = kTextVA + 0x100;
  T.writeText(Byte, {0x49, 0x3B, 0x66, 0x10, 0x76, 0x03, 0x89, 0xC8, 0xC3, 0x88,
                     0x4C, 0x24, 0x08});
  T.writeText(Byte + 0x0D, makeCall(Byte + 0x0D, Morestack));
  T.writeText(Byte + 0x12, {0x0F, 0xB6, 0x4C, 0x24, 0x08, 0xEB, 0xE7});
  T.writeText(Morestack, {0xC3});
  GoModuleInfo Module;
  Module.RegisterArguments = true;
  Module.GoCode = {{Byte, kTextVA + 0x500}};
  Module.MorestackEntries = {Morestack};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  const auto Spills = goMorestackSpills(T.Img, build(T.Img, Byte).Blocks);
  ASSERT_TRUE(Spills.has_value());
  ASSERT_EQ(Spills->size(), 1u);
  EXPECT_EQ((*Spills)[0].Register, x86reg::RCX);
  EXPECT_EQ((*Spills)[0].Size, 1u);
  EXPECT_EQ((*Spills)[0].EntryOffset, 8);
}

TEST(GoRegisterArguments, PaddingInTheGrowthPathKeepsItsShape) {
  // main.p: cmp rsp, [r14+0x10]; jbe grow; ret
  //   grow: mov [rsp+8], rax; nop; call morestack_noctxt
  //         mov rax, [rsp+8]; xchg ax, ax; jmp main.p
  GoTestImage T;
  constexpr va_t Padded = kTextVA + 0x100;
  T.writeText(Padded, {0x49, 0x3B, 0x66, 0x10, 0x76, 0x01, 0xC3, 0x48, 0x89,
                       0x44, 0x24, 0x08, 0x90});
  T.writeText(Padded + 0x0D, makeCall(Padded + 0x0D, Morestack));
  T.writeText(Padded + 0x12,
              {0x48, 0x8B, 0x44, 0x24, 0x08, 0x66, 0x90, 0xEB, 0xE5});
  T.writeText(Morestack, {0xC3});
  GoModuleInfo Module;
  Module.RegisterArguments = true;
  Module.GoCode = {{Padded, kTextVA + 0x500}};
  Module.MorestackEntries = {Morestack};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  const auto Spills = goMorestackSpills(T.Img, build(T.Img, Padded).Blocks);
  ASSERT_TRUE(Spills.has_value());
  ASSERT_EQ(Spills->size(), 1u);
  EXPECT_EQ((*Spills)[0].Register, x86reg::RAX);
  // The growth routine itself takes no register argument.
  const LocalRegisterEffect Grow = localRegisterEffect(
      T.Img, build(T.Img, Morestack), nullptr, nullptr, nullptr, nullptr);
  ASSERT_TRUE(Grow.GoArgumentReads.has_value());
  EXPECT_EQ(*Grow.GoArgumentReads, GPRReadWidths{});
}

TEST(GoRegisterArguments, AGoFunctionTakesThemInGoRegisters) {
  const BinaryImage Img = registerArgumentImage();
  EXPECT_TRUE(usesGoRegisterArguments(Img, GoCallee));
  EXPECT_FALSE(usesGoRegisterArguments(Img, Assembly));
  const MedFunc F = lift(Img, GoCallee);
  ASSERT_FALSE(F.IntegerArgumentRegisters.empty());
  EXPECT_EQ(F.IntegerArgumentRegisters.front(), x86reg::RAX);
  // RAX and RBX, and no stack parameter read back from the spill area.
  ASSERT_EQ(F.Params.size(), 2u);
  EXPECT_EQ(F.Params[0].RegOff, x86reg::RAX);
  EXPECT_EQ(F.Params[1].RegOff, x86reg::RBX);

  const LocalRegisterEffect Effect = localRegisterEffect(
      Img, build(Img, GoCallee), nullptr, nullptr, nullptr, nullptr);
  ASSERT_TRUE(Effect.GoArgumentReads.has_value());
  EXPECT_EQ((*Effect.GoArgumentReads)[x86reg::RAX / 8], 8u);
  EXPECT_EQ((*Effect.GoArgumentReads)[x86reg::RBX / 8], 8u);
  EXPECT_EQ((*Effect.GoArgumentReads)[x86reg::RDI / 8], 0u);
  EXPECT_EQ(Effect.StackArgs, 0);
}

TEST(GoRegisterArguments, ABoolFirstArgumentIsNoVectorCount) {
  // System V reads a summary that reads one byte of RAX as a variadic
  // prologue's vector count, which lists no parameters.  A Go function's
  // first argument can be a bool: printhexopts(include0x bool, ...) in AL.
  const BinaryImage Img = registerArgumentImage();
  GPRReadWidths Reads{};
  Reads[x86reg::RAX / 8] = 1;
  Reads[x86reg::RBX / 8] = 8;
  const std::map<va_t, GPRReadWidths> Summaries{{GoCallee, Reads}};
  LowToMedConverter Caller;
  Caller.setBinaryImage(&Img);
  Caller.setCallEntryReadGPRs(&Summaries);
  const MedFunc G = Caller.convert(build(Img, GoCaller), Img.Arch, Img.Format);
  const MedOp *Call = onlyCall(G);
  ASSERT_NE(Call, nullptr);
  ASSERT_EQ(Call->NumInputs, 3u);
  // The callee's own parameters follow the same summary.
  LowToMedConverter Callee;
  Callee.setBinaryImage(&Img);
  Callee.setCallEntryReadGPRs(&Summaries);
  const MedFunc F = Callee.convert(build(Img, GoCallee), Img.Arch, Img.Format);
  ASSERT_EQ(F.Params.size(), 2u);
  EXPECT_EQ(F.Params[0].RegOff, x86reg::RAX);
  EXPECT_EQ(F.Params[1].RegOff, x86reg::RBX);
}

/// Whether \p F has an operation of opcode \p Opcode.
bool hasOp(const MedFunc &F, NdOp Opcode) {
  for (const MedBlock &Block : F.Blocks)
    for (const MedOp &Op : Block.Ops)
      if (Op.Opcode == Opcode)
        return true;
  return false;
}

TEST(GoStackGrowth, TheFunctionEntersItsBodyDirectly) {
  // The growth path restarts main.f with the arguments it spilled, so the
  // body is all main.f computes: no morestack call, no stack check branch,
  // and the entry goes straight on to `lea rax, [rax+rbx]`.
  const BinaryImage Img = registerArgumentImage();
  const MedFunc F = lift(Img, GoCallee);
  EXPECT_FALSE(hasOp(F, NdOp::CALL));
  EXPECT_FALSE(hasOp(F, NdOp::COND_BR));
  ASSERT_EQ(F.Params.size(), 2u);
  EXPECT_EQ(F.Params[0].RegOff, x86reg::RAX);
  EXPECT_EQ(F.Params[1].RegOff, x86reg::RBX);
}

/// A Go function at kTextVA + 0x100 whose entry is \p Check, then `ret`,
/// and whose growth path is `call morestack_noctxt; jmp entry`, as Go 1.16
/// and earlier emit it: nothing is spilled.
BinaryImage stackGrowthImage(std::vector<uint8_t> Check) {
  constexpr va_t Function = kTextVA + 0x100;
  GoTestImage T;
  const uint8_t Length = static_cast<uint8_t>(Check.size());
  Check.insert(Check.end(), {0x76, 0x01, 0xC3});
  T.writeText(Function, Check);
  const va_t Grow = Function + Length + 3;
  T.writeText(Grow, makeCall(Grow, Morestack));
  T.writeText(Grow + 5, {0xEB, static_cast<uint8_t>(Function - (Grow + 7))});
  T.writeText(Morestack, {0xC3});
  GoModuleInfo Module;
  Module.GoCode = {{Function, kTextVA + 0x500}};
  Module.MorestackEntries = {Morestack};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  return std::move(T.Img);
}

TEST(GoStackGrowth, GReadFromThreadLocalStorageChecksTheSameWay) {
  // mov rcx, fs:[-8]; cmp rsp, [rcx+0x10]; jbe grow
  const BinaryImage Img =
      stackGrowthImage({0x64, 0x48, 0x8B, 0x0C, 0x25, 0xF8, 0xFF, 0xFF, 0xFF,
                        0x48, 0x3B, 0x61, 0x10});
  const auto Growth = goStackGrowth(Img, build(Img, kTextVA + 0x100).Blocks);
  ASSERT_TRUE(Growth.has_value());
  EXPECT_TRUE(Growth->Spills.empty());
  EXPECT_TRUE(Growth->Restarts);
  EXPECT_FALSE(hasOp(lift(Img, kTextVA + 0x100), NdOp::CALL));
}

TEST(GoStackGrowth, AnEntryThatStoresIsNoStackCheck) {
  // mov [rsp+8], rax; cmp rsp, [r14+0x10]; jbe grow: running the entry again
  // would store again, so the path is not one that changes nothing.
  const BinaryImage Img =
      stackGrowthImage({0x48, 0x89, 0x44, 0x24, 0x08, 0x49, 0x3B, 0x66, 0x10});
  EXPECT_FALSE(goStackGrowth(Img, build(Img, kTextVA + 0x100).Blocks));
  EXPECT_TRUE(hasOp(lift(Img, kTextVA + 0x100), NdOp::CALL));
}

TEST(GoStackGrowth, ARoutineThatThrowsKeepsItsPath) {
  // runtime.morestackc throws: a system-stack function that runs short of
  // stack never starts again, so its path stays.
  BinaryImage Img = registerArgumentImage();
  Img.ExceptionMetadata.GoModule->ThrowingMorestackEntries = {Morestack};
  const MedFunc F = lift(Img, GoCallee);
  EXPECT_TRUE(hasOp(F, NdOp::CALL));
}

TEST(GoRegisterArguments, ACallPassesThemInGoRegisters) {
  const BinaryImage Img = registerArgumentImage();
  GPRReadWidths Reads{};
  Reads[x86reg::RAX / 8] = 8;
  Reads[x86reg::RBX / 8] = 8;
  const std::map<va_t, GPRReadWidths> Summaries{{GoCallee, Reads}};
  LowToMedConverter Converter;
  Converter.setBinaryImage(&Img);
  Converter.setCallEntryReadGPRs(&Summaries);
  const MedFunc F =
      Converter.convert(build(Img, GoCaller), Img.Arch, Img.Format);
  const MedOp *Call = onlyCall(F);
  ASSERT_NE(Call, nullptr);
  ASSERT_EQ(Call->NumInputs, 3u);
  // The values the caller set in RAX and RBX, not System V's RDI and RSI.
  for (unsigned I = 1; I < 3; ++I) {
    const MedVar &Argument = Call->Inputs[I];
    if (Argument.Kind == MedVar::Const)
      EXPECT_EQ(Argument.ConstVal, I);
    else
      EXPECT_EQ(Argument.RegOff, I == 1 ? x86reg::RAX : x86reg::RBX);
  }
}

} // namespace
