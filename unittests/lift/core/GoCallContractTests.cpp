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

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/CallRegisterEffects.h"
#include "neverd/ir/low/FuncDetector.h"
#include "neverd/ir/low/GoCallContract.h"
#include "neverd/ir/low/GoInlinedRoutine.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/MedCallLeftovers.h"
#include "neverd/ir/med/MedGoResults.h"
#include "neverd/ir/med/MedPrologueSaves.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include <array>
#include <cstring>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <tuple>
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
  EXPECT_TRUE(F.ExactRegisterParameters);
  // A caller without a stack-growth path proves none.
  EXPECT_FALSE(lift(Img, GoCaller).ExactRegisterParameters);

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

/// A Go function at kTextVA + 0x100 whose frame is too large for one
/// comparison, as Go's assembler checks it (`stacksplit`): it proves that the
/// stack pointer less the frame does not wrap, then compares that with the
/// guard, and each branch leaves for the growth path.  \p Between runs before
/// the second comparison.
///   mov r14, fs:[-8]; mov r12, rsp; sub r12, 0xfff88; jb grow; <Between>
///   cmp r12, [r14+0x10]; jbe grow; ret
///   grow: call morestack_noctxt; jmp entry
BinaryImage hugeFrameImage(std::vector<uint8_t> Between = {}) {
  constexpr va_t Function = kTextVA + 0x100;
  GoTestImage T;
  std::vector<uint8_t> Code = {0x64, 0x4C, 0x8B, 0x34, 0x25, 0xF8, 0xFF,
                               0xFF, 0xFF, 0x49, 0x89, 0xE4, 0x49, 0x81,
                               0xEC, 0x88, 0xFF, 0x0F, 0x00, 0x72};
  const auto Rest = static_cast<uint8_t>(Between.size() + 7);
  Code.push_back(Rest);
  Code.insert(Code.end(), Between.begin(), Between.end());
  Code.insert(Code.end(), {0x4D, 0x3B, 0x66, 0x10, 0x76, 0x01, 0xC3});
  T.writeText(Function, Code);
  const va_t Grow = Function + Code.size();
  T.writeText(Grow, makeCall(Grow, Morestack));
  T.writeText(Grow + 5, {0xEB, static_cast<uint8_t>(Function - (Grow + 7))});
  T.writeText(Morestack, {0xC3});
  GoModuleInfo Module;
  Module.GoCode = {{Function, kTextVA + 0x500}};
  Module.MorestackEntries = {Morestack};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  return std::move(T.Img);
}

TEST(GoStackGrowth, AFrameTooLargeForOneComparisonChecksTwice) {
  // Both branches leave for the one growth path, which restarts the
  // function: the body is all it computes, as with one comparison.
  // Reflection's `runtime.callNNN` printed theirs under the two conditions.
  const BinaryImage Img = hugeFrameImage();
  const auto Growth = goStackGrowth(Img, build(Img, kTextVA + 0x100).Blocks);
  ASSERT_TRUE(Growth.has_value());
  EXPECT_TRUE(Growth->Restarts);
  EXPECT_NE(Growth->SecondCheck, InvalidVA);
  const MedFunc F = lift(Img, kTextVA + 0x100);
  EXPECT_FALSE(hasOp(F, NdOp::CALL));
  EXPECT_FALSE(hasOp(F, NdOp::COND_BR));
  // A second block that stores is no part of a check: the path stays.
  //   mov [rsp+8], rax
  const BinaryImage Storing = hugeFrameImage({0x48, 0x89, 0x44, 0x24, 0x08});
  const auto Kept =
      goStackGrowth(Storing, build(Storing, kTextVA + 0x100).Blocks);
  if (Kept)
    EXPECT_EQ(Kept->SecondCheck, InvalidVA);
  EXPECT_TRUE(hasOp(lift(Storing, kTextVA + 0x100), NdOp::CALL));
}

TEST(GoStackGrowth, AnEntryThatStoresIsNoStackCheck) {
  // mov [rsp+8], rax; cmp rsp, [r14+0x10]; jbe grow: running the entry again
  // would store again, so the path is not one that changes nothing.
  const BinaryImage Img =
      stackGrowthImage({0x48, 0x89, 0x44, 0x24, 0x08, 0x49, 0x3B, 0x66, 0x10});
  EXPECT_FALSE(goStackGrowth(Img, build(Img, kTextVA + 0x100).Blocks));
  EXPECT_TRUE(hasOp(lift(Img, kTextVA + 0x100), NdOp::CALL));
}

/// Little-endian bytes of AArch64 instruction words.
std::vector<uint8_t> words(std::initializer_list<uint32_t> Words) {
  std::vector<uint8_t> Bytes;
  for (const uint32_t W : Words)
    for (unsigned I = 0; I < 4; ++I)
      Bytes.push_back(static_cast<uint8_t>(W >> (8 * I)));
  return Bytes;
}

TEST(GoStackGrowth, AnARM64PathRestartsTheFunctionToo) {
  // main.f: ldr x16, [x28, #16]; cmp sp, x16; b.ls grow; add x0, x0, x1; ret
  //   grow: stp x0, x1, [sp, #8]; mov x3, x30; bl morestack_noctxt
  //         ldp x0, x1, [sp, #8]; b main.f
  // The growth routine ends in a trap, as Go's does after newstack; the call
  // resumes at its return address all the same.
  constexpr va_t Function = kTextVA + 0x100;
  GoTestImage T(Arch::AArch64);
  const uint32_t Call =
      0x94000000u | (((Morestack - (Function + 0x1C)) >> 2) & 0x3FFFFFFu);
  T.writeText(Function,
              words({0xF9400B90, 0xEB3063FF, 0x54000069, 0x8B010000, 0xD65F03C0,
                     0xA90087E0, 0xAA1E03E3, Call, 0xA94087E0, 0x17FFFFF7}));
  T.writeText(Morestack, words({0x00000000}));
  GoModuleInfo Module;
  Module.RegisterArguments = true;
  Module.GoCode = {{Function, kTextVA + 0x500}};
  Module.MorestackEntries = {Morestack};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  EXPECT_TRUE(goCallResumesAfter(T.Img, Morestack));
  const auto Growth = goStackGrowth(T.Img, build(T.Img, Function).Blocks);
  ASSERT_TRUE(Growth.has_value());
  ASSERT_EQ(Growth->Spills.size(), 2u);
  EXPECT_EQ(Growth->Spills[0].Register, a64reg::X0);
  EXPECT_EQ(Growth->Spills[0].EntryOffset, 8);
  EXPECT_EQ(Growth->Spills[1].Register, a64reg::X1);
  EXPECT_TRUE(Growth->Restarts);
  const MedFunc F = lift(T.Img, Function);
  EXPECT_FALSE(hasOp(F, NdOp::CALL));
  EXPECT_FALSE(hasOp(F, NdOp::COND_BR));
  // Its parameters are the two registers the path spills, in Go's order,
  // and call recovery adds none to them.
  ASSERT_EQ(F.Params.size(), 2u);
  EXPECT_EQ(F.Params[0].RegOff, a64reg::X0);
  EXPECT_EQ(F.Params[1].RegOff, a64reg::X1);
  EXPECT_TRUE(F.ExactRegisterParameters);
}

TEST(GoStackGrowth, AnARM64StackABIPathRestartsTheFunction) {
  // Go 1.17 and earlier pass arm64 arguments on the stack, so the path spills
  // nothing.  Kept, it rejoined the entry with the link register and the
  // goroutine `bl morestack_noctxt` left there.
  //   main.f: ldr x1, [x28, #16]; mov x2, sp; cmp x2, x1; b.ls grow
  //           ldr x0, [sp, #8]; ret
  //     grow: mov x3, x30; bl morestack_noctxt; b main.f
  constexpr va_t Function = kTextVA + 0x100;
  GoTestImage T(Arch::AArch64);
  const uint32_t Call =
      0x94000000u | (((Morestack - (Function + 0x1C)) >> 2) & 0x3FFFFFFu);
  T.writeText(Function,
              words({0xF9400B81, 0x910003E2, 0xEB01005F, 0x54000069, 0xF94007E0,
                     0xD65F03C0, 0xAA1E03E3, Call, 0x17FFFFF8}));
  T.writeText(Morestack, words({0x00000000}));
  GoModuleInfo Module;
  Module.StackArguments = true;
  Module.GoCode = {{Function, kTextVA + 0x500}};
  Module.MorestackEntries = {Morestack};
  Module.FrameArguments = {{Function, 8}};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  EXPECT_TRUE(goCallResumesAfter(T.Img, Morestack));
  const auto Growth = goStackGrowth(T.Img, build(T.Img, Function).Blocks);
  ASSERT_TRUE(Growth.has_value());
  EXPECT_TRUE(Growth->Spills.empty());
  EXPECT_TRUE(Growth->Restarts);
  const MedFunc F = lift(T.Img, Function);
  EXPECT_FALSE(hasOp(F, NdOp::CALL));
  EXPECT_FALSE(hasOp(F, NdOp::COND_BR));
}

TEST(GoStackGrowth, AGrowthRoutineTakesNoArgument) {
  // A growth path hands runtime.morestack_noctxt its own return address in
  // R3, as a call does in LR, and the routines' other reads save registers.
  // Read as AAPCS64 positions, every arm64 growth call passed four or more
  // unknowns.
  //   runtime.morestack_noctxt: mov w26, #0; b runtime.morestack
  //   runtime.morestack: mov x16, x3; str x0, [x28, #0x50]; udf
  // The routine reads R3 itself, which is no argument either.
  constexpr va_t Noctxt = Morestack + 0x40;
  GoTestImage T(Arch::AArch64);
  T.writeText(Morestack, words({0xAA0303F0, 0xF9002B80, 0x00000000}));
  const uint32_t Branch =
      0x14000000u | (((Morestack - (Noctxt + 4)) >> 2) & 0x3FFFFFFu);
  T.writeText(Noctxt, words({0x5280001A, Branch}));
  for (const auto &[Entry, Name] :
       {std::pair{Morestack, "runtime.morestack"},
        std::pair{Noctxt, "runtime.morestack_noctxt"}}) {
    Symbol Function = Symbol::makeFunc(Entry);
    Function.Name = Name;
    T.Img.Symbols.push_back(std::move(Function));
  }
  GoModuleInfo Module;
  Module.PclnTabMagic = 0xFFFFFFF1u;
  Module.RegisterArguments = true;
  Module.GoCode = {{kTextVA, kTextVA + 0x500}};
  Module.MorestackEntries = {Morestack, Noctxt};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  EXPECT_TRUE(lift(T.Img, Morestack).Params.empty());
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  llvm::LLVMContext Context;
  const PipelineResult R = Pipeline().run(T.Img, Context, Options);
  ASSERT_TRUE(R.Success) << R.Error;
  for (const va_t Entry : {Morestack, Noctxt}) {
    SCOPED_TRACE(Entry);
    const auto It = llvm::find_if(
        R.MedFuncs, [&](const MedFunc &M) { return M.Entry == Entry; });
    ASSERT_NE(It, R.MedFuncs.end());
    EXPECT_TRUE(It->Params.empty()) << It->Params.size();
    EXPECT_TRUE(It->ExactRegisterParameters);
  }
}

TEST(GoStackGrowth, AnARM64ByteSpillGoesThroughATemporary) {
  // main.g(b bool): ldr x16, [x28, #16]; cmp sp, x16; b.ls grow; ret
  //   grow: strb w2, [sp, #24]; mov x3, x30; bl morestack_noctxt
  //         ldrb w2, [sp, #24]; b main.g
  // arm64 stores the byte through a temporary of W2's low byte.
  constexpr va_t Function = kTextVA + 0x100;
  GoTestImage T(Arch::AArch64);
  const uint32_t Call =
      0x94000000u | (((Morestack - (Function + 0x18)) >> 2) & 0x3FFFFFFu);
  T.writeText(Function,
              words({0xF9400B90, 0xEB3063FF, 0x54000049, 0xD65F03C0, 0x390063E2,
                     0xAA1E03E3, Call, 0x394063E2, 0x17FFFFF8}));
  T.writeText(Morestack, words({0x00000000}));
  GoModuleInfo Module;
  Module.RegisterArguments = true;
  Module.GoCode = {{Function, kTextVA + 0x500}};
  Module.MorestackEntries = {Morestack};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  const auto Growth = goStackGrowth(T.Img, build(T.Img, Function).Blocks);
  ASSERT_TRUE(Growth.has_value());
  ASSERT_EQ(Growth->Spills.size(), 1u);
  EXPECT_EQ(Growth->Spills[0].Register, a64reg::X2);
  EXPECT_EQ(Growth->Spills[0].Size, 1u);
  EXPECT_EQ(Growth->Spills[0].EntryOffset, 24);
}

TEST(GoStackGrowth, AFloatSpillIsAnArgumentToo) {
  // runtime.modf: cmp rsp, [r14+0x10]; jbe grow; ret
  //   grow: movsd [rsp+8], xmm0; call morestack_noctxt
  //         movsd xmm0, [rsp+8]; jmp runtime.modf
  // The path spills the scalar the argument is, through a temporary of
  // XMM0's low eight bytes, and names the function's parameters exactly: a
  // double in X0, though the body never reads it.  Unmodelled, the path
  // stayed in the function as a loop round a call of morestack.
  constexpr va_t Function = kTextVA + 0x100;
  GoTestImage T;
  T.writeText(Function, {0x49, 0x3B, 0x66, 0x10, 0x76, 0x01, 0xC3, 0xF2, 0x0F,
                         0x11, 0x44, 0x24, 0x08});
  T.writeText(Function + 0x0D, makeCall(Function + 0x0D, Morestack));
  T.writeText(Function + 0x12,
              {0xF2, 0x0F, 0x10, 0x44, 0x24, 0x08, 0xEB, 0xE6});
  T.writeText(Morestack, {0xC3});
  GoModuleInfo Module;
  Module.RegisterArguments = true;
  Module.GoCode = {{Function, kTextVA + 0x500}};
  Module.MorestackEntries = {Morestack};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  const auto Growth = goStackGrowth(T.Img, build(T.Img, Function).Blocks);
  ASSERT_TRUE(Growth.has_value());
  ASSERT_EQ(Growth->Spills.size(), 1u);
  EXPECT_EQ(Growth->Spills[0].Register, x86reg::vectorReg(0));
  EXPECT_EQ(Growth->Spills[0].Size, 8u);
  EXPECT_EQ(Growth->Spills[0].EntryOffset, 8);
  const MedFunc F = lift(T.Img, Function);
  EXPECT_TRUE(F.ExactRegisterParameters);
  ASSERT_EQ(F.Params.size(), 1u);
  EXPECT_EQ(F.Params[0].RegOff, x86reg::vectorReg(0));
  const auto Scalar = F.FPParamScalarBytes.find(x86reg::vectorReg(0));
  ASSERT_NE(Scalar, F.FPParamScalarBytes.end());
  EXPECT_EQ(Scalar->second, 8u);
  // Its summary reads the double for its callers.
  const LocalRegisterEffect Effect = localRegisterEffect(
      T.Img, build(T.Img, Function), nullptr, nullptr, nullptr, nullptr);
  ASSERT_TRUE(Effect.GoArgumentReads.has_value());
  EXPECT_EQ((*Effect.GoArgumentReads)[kX64VectorFamilyBase], 8u);
}

TEST(GoStackGrowth, ASpillOfANinthFloatingRegisterIsNotModelled) {
  // The same path over XMM8: `movsd [rsp+8], xmm8 ... movsd xmm8, [rsp+8]`.
  // A summary's vector rows name the first eight floating registers, so a
  // path that spills a later one names an argument list no call completes.
  constexpr va_t Function = kTextVA + 0x100;
  GoTestImage T;
  T.writeText(Function, {0x49, 0x3B, 0x66, 0x10, 0x76, 0x01, 0xC3, 0xF2, 0x44,
                         0x0F, 0x11, 0x44, 0x24, 0x08});
  T.writeText(Function + 0x0E, makeCall(Function + 0x0E, Morestack));
  T.writeText(Function + 0x13,
              {0xF2, 0x44, 0x0F, 0x10, 0x44, 0x24, 0x08, 0xEB, 0xE4});
  T.writeText(Morestack, {0xC3});
  GoModuleInfo Module;
  Module.RegisterArguments = true;
  Module.GoCode = {{Function, kTextVA + 0x500}};
  Module.MorestackEntries = {Morestack};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  EXPECT_FALSE(goStackGrowth(T.Img, build(T.Img, Function).Blocks));
  EXPECT_FALSE(lift(T.Img, Function).ExactRegisterParameters);
}

TEST(GoWriteBarriers, ABufferBarrierTakesNoRegisterArgument) {
  // runtime.gcWriteBarrier2 on arm64: stp x0, x1, [sp, #-16]!;
  // mov x25, sp; ldp x0, x1, [sp], #16; ret.  Its saves read no argument:
  // it returns buffer space in X25 and keeps every other register.
  constexpr va_t BarrierEntry = kTextVA + 0x200;
  GoTestImage T(Arch::AArch64);
  T.writeText(BarrierEntry,
              words({0xA9BF07E0, 0x910003F9, 0xA8C107E0, 0xD65F03C0}));
  GoModuleInfo Module;
  Module.RegisterArguments = true;
  Module.GoCode = {{kTextVA + 0x100, kTextVA + 0x500}};
  Module.GoAssembly = {{BarrierEntry, BarrierEntry + 0x10}};
  GoWriteBarrier Barrier;
  Barrier.Entry = BarrierEntry;
  Barrier.Kind = GoWriteBarrierKind::Buffer;
  Module.WriteBarriers = {Barrier};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  const MedFunc F = lift(T.Img, BarrierEntry);
  EXPECT_TRUE(F.Params.empty());
  EXPECT_TRUE(F.ExactRegisterParameters);
}

TEST(GoWriteBarriers, AStoringBarrierTakesItsAddressAndPointer) {
  // A Go 1.11-1.20 barrier takes the address it writes at and the pointer it
  // writes there in registers of its own: RDI and the register its name gives
  // (`runtime.gcWriteBarrierCX`), or arm64's X2 and X3.  Read by position, a
  // System V call passed `gcWriteBarrierCX(slot, 0, 0, value)`, a Win64 call
  // `(value)`, and an AAPCS64 call `(x0, <placeholder>, slot, value)`.
  //   main.f: mov edi, 0x1000; mov ecx, 0x2000; call barrier; ret
  //   barrier: mov [rdi], rcx; ret
  //   arm64: mov x2, #0x1000; mov x3, #0x2000; bl barrier; ret
  //          barrier: str x3, [x2]; ret
  constexpr va_t Caller = kTextVA + 0x100;
  constexpr va_t BarrierEntry = kTextVA + 0x200;
  struct Shape {
    Arch A;
    BinaryFormat Format;
    bool StackArguments;
  };
  for (const Shape &Case : {Shape{Arch::X64, BinaryFormat::ELF, false},
                            Shape{Arch::X64, BinaryFormat::COFF, false},
                            Shape{Arch::X64, BinaryFormat::ELF, true},
                            Shape{Arch::AArch64, BinaryFormat::ELF, false}})
    for (const bool Restricted : {false, true}) {
      SCOPED_TRACE(testing::Message()
                   << "arm64=" << (Case.A == Arch::AArch64)
                   << " pe=" << (Case.Format == BinaryFormat::COFF) << " stack="
                   << Case.StackArguments << " restricted=" << Restricted);
      GoTestImage T(Case.A);
      T.Img.Format = Case.Format;
      GoWriteBarrier Barrier;
      Barrier.Entry = BarrierEntry;
      Barrier.Kind = GoWriteBarrierKind::Store;
      if (Case.A == Arch::X64) {
        std::vector<uint8_t> Body = {0xBF, 0x00, 0x10, 0x00, 0x00,
                                     0xB9, 0x00, 0x20, 0x00, 0x00};
        const std::vector<uint8_t> Call =
            makeCall(Caller + Body.size(), BarrierEntry);
        Body.insert(Body.end(), Call.begin(), Call.end());
        Body.push_back(0xC3);
        T.writeText(Caller, Body);
        T.writeText(BarrierEntry, {0x48, 0x89, 0x0F, 0xC3});
        Barrier.Destination = x86reg::RDI;
        Barrier.Value = x86reg::RCX;
      } else {
        const uint32_t Call =
            0x94000000u | (((BarrierEntry - (Caller + 8)) >> 2) & 0x3FFFFFFu);
        T.writeText(Caller, words({0xD2820002, 0xD2840003, Call, 0xD65F03C0}));
        T.writeText(BarrierEntry, words({0xF9000043, 0xD65F03C0}));
        Barrier.Destination = a64reg::X2;
        Barrier.Value = a64reg::X3;
      }
      GoModuleInfo Module;
      Module.RegisterArguments = !Case.StackArguments;
      Module.StackArguments = Case.StackArguments;
      Module.GoCode = {{Caller, kTextVA + 0x300}};
      Module.GoAssembly = {{BarrierEntry, kTextVA + 0x300}};
      Module.WriteBarriers = {Barrier};
      T.Img.ExceptionMetadata.GoModule = std::move(Module);
      for (const auto &[Entry, Name] :
           {std::pair{Caller, "main.f"},
            std::pair{BarrierEntry, "runtime.gcWriteBarrierCX"}}) {
        Symbol Function = Symbol::makeFunc(Entry);
        Function.Name = Name;
        T.Img.Symbols.push_back(std::move(Function));
      }
      const std::optional<FixedIntegerCallArguments> Operands =
          goWriteBarrierArguments(T.Img, BarrierEntry);
      ASSERT_TRUE(Operands.has_value());
      ASSERT_EQ(Operands->Parameters.size(), 2u);
      EXPECT_EQ(Operands->Parameters[0].Register, Barrier.Destination);
      EXPECT_EQ(Operands->Parameters[1].Register, Barrier.Value);

      PipelineOptions Options;
      Options.EmitDumpOutput = false;
      if (Restricted)
        Options.OnlyFunctionEntries = {Caller};
      llvm::LLVMContext Context;
      const PipelineResult R = Pipeline().run(T.Img, Context, Options);
      ASSERT_TRUE(R.Success) << R.Error;
      const auto F = llvm::find_if(
          R.MedFuncs, [&](const MedFunc &M) { return M.Entry == Caller; });
      ASSERT_NE(F, R.MedFuncs.end());
      // The call binds the two registers before SSA, and a recovery of its
      // operands, which an image with the stack ABI runs, keeps them.
      auto IsOperand = [](const MedVar &V, uint64_t Constant,
                          uint64_t Register) {
        return V.isConst() ? V.ConstVal == Constant
                           : V.Kind == MedVar::Reg && V.RegOff == Register;
      };
      const MedOp *Call = onlyCall(*F);
      ASSERT_NE(Call, nullptr);
      ASSERT_EQ(Call->NumInputs, 3u);
      EXPECT_TRUE(IsOperand(Call->Inputs[1], 0x1000, Barrier.Destination));
      EXPECT_TRUE(IsOperand(Call->Inputs[2], 0x2000, Barrier.Value));
      if (!F->CallInfos.empty()) {
        ASSERT_EQ(F->CallInfos.size(), 1u);
        const std::vector<MedVar> &Arguments = F->CallInfos[0].Args;
        ASSERT_EQ(Arguments.size(), 2u);
        EXPECT_TRUE(IsOperand(Arguments[0], 0x1000, Barrier.Destination));
        EXPECT_TRUE(IsOperand(Arguments[1], 0x2000, Barrier.Value));
      }
      if (Restricted)
        continue;
      // The barrier's own parameters are the same two registers.
      const auto B = llvm::find_if(R.MedFuncs, [&](const MedFunc &M) {
        return M.Entry == BarrierEntry;
      });
      ASSERT_NE(B, R.MedFuncs.end());
      ASSERT_EQ(B->Params.size(), 2u);
      EXPECT_EQ(B->Params[0].RegOff, Barrier.Destination);
      EXPECT_EQ(B->Params[1].RegOff, Barrier.Value);
      EXPECT_TRUE(B->ExactRegisterParameters);
    }
}

TEST(GoStackGrowth, ARoutineThatThrowsKeepsItsPath) {
  // runtime.morestackc throws: a system-stack function that runs short of
  // stack never starts again, so its path stays.
  BinaryImage Img = registerArgumentImage();
  Img.ExceptionMetadata.GoModule->ThrowingMorestackEntries = {Morestack};
  const MedFunc F = lift(Img, GoCallee);
  EXPECT_TRUE(hasOp(F, NdOp::CALL));
}

constexpr va_t AssemblyCaller = kTextVA + 0x280;

/// main.h calls the assembly main.add, which takes its arguments in Go's
/// registers as runtime.memmove does:
///
///   main.h:   mov eax, 1; mov ebx, 2; call main.add; ret
///   main.add: lea rax, [rax+rbx]; ret
BinaryImage assemblyImage() {
  GoTestImage T;
  T.writeText(AssemblyCaller, {0xB8, 1, 0, 0, 0, 0xBB, 2, 0, 0, 0});
  T.writeText(AssemblyCaller + 0x0A, makeCall(AssemblyCaller + 0x0A, Assembly));
  T.writeText(AssemblyCaller + 0x0F, {0xC3});
  T.writeText(Assembly, {0x48, 0x8D, 0x04, 0x18, 0xC3});
  GoModuleInfo Module;
  Module.RegisterArguments = true;
  Module.GoCode = {{GoCallee, kTextVA + 0x500}};
  Module.GoAssembly = {{Assembly, kTextVA + 0x500}};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  return std::move(T.Img);
}

GPRReadWidths reads(std::initializer_list<uint64_t> Registers) {
  GPRReadWidths Reads{};
  for (const uint64_t Register : Registers)
    Reads[Register / 8] = 8;
  return Reads;
}

TEST(GoAssembly, ARunOfGoArgumentRegistersIsItsArguments) {
  const BinaryImage Img = assemblyImage();
  using namespace x86reg;
  // memmove(to, from, n), and memequal_varlen, which reads its closure.
  EXPECT_TRUE(
      goAssemblyTakesRegisterArguments(Img, Assembly, reads({RAX, RBX, RCX})));
  EXPECT_TRUE(goAssemblyTakesRegisterArguments(Img, Assembly,
                                               reads({RAX, RBX, RDX, R14})));
  // rt0_go reads the process's RDI and RSI; spillArgs reads R12 as well;
  // a skipped argument register is no run; nothing read is no argument.
  EXPECT_FALSE(
      goAssemblyTakesRegisterArguments(Img, Assembly, reads({RDI, RSI})));
  EXPECT_FALSE(
      goAssemblyTakesRegisterArguments(Img, Assembly, reads({RAX, R12})));
  EXPECT_FALSE(
      goAssemblyTakesRegisterArguments(Img, Assembly, reads({RAX, RCX})));
  EXPECT_FALSE(goAssemblyTakesRegisterArguments(Img, Assembly, reads({})));
  // Compiled code answers to usesGoRegisterArguments instead.
  EXPECT_FALSE(
      goAssemblyTakesRegisterArguments(Img, AssemblyCaller, reads({RAX, RBX})));
}

TEST(GoAssembly, ACallPassesItsArgumentsInGoRegisters) {
  const BinaryImage Img = assemblyImage();
  const std::map<va_t, GPRReadWidths> Summaries{
      {Assembly, reads({x86reg::RAX, x86reg::RBX})}};
  LowToMedConverter Caller;
  Caller.setBinaryImage(&Img);
  Caller.setCallEntryReadGPRs(&Summaries);
  const MedFunc H =
      Caller.convert(build(Img, AssemblyCaller), Img.Arch, Img.Format);
  const MedOp *Call = onlyCall(H);
  ASSERT_NE(Call, nullptr);
  // System V's first slots are RDI and RSI, which main.add never reads.
  ASSERT_EQ(Call->NumInputs, 3u);
  for (unsigned I = 1; I < 3; ++I) {
    const MedVar &Argument = Call->Inputs[I];
    if (Argument.Kind == MedVar::Const)
      EXPECT_EQ(Argument.ConstVal, I);
    else
      EXPECT_EQ(Argument.RegOff, I == 1 ? x86reg::RAX : x86reg::RBX);
  }
  LowToMedConverter Callee;
  Callee.setBinaryImage(&Img);
  Callee.setCallEntryReadGPRs(&Summaries);
  const MedFunc Add =
      Callee.convert(build(Img, Assembly), Img.Arch, Img.Format);
  ASSERT_FALSE(Add.IntegerArgumentRegisters.empty());
  EXPECT_EQ(Add.IntegerArgumentRegisters.front(), x86reg::RAX);
  ASSERT_EQ(Add.Params.size(), 2u);
  EXPECT_EQ(Add.Params[0].RegOff, x86reg::RAX);
  EXPECT_EQ(Add.Params[1].RegOff, x86reg::RBX);
}

TEST(GoAssembly, ABI0AssemblyTakesItsArgumentsOnTheStack) {
  // ArgumentsAndTheirCallsAreOnTheStack's main.add and its caller in a Go 1.18
  // image, main.add marked as assembly: Go assembly not declared
  // <ABIInternal> keeps the stack ABI (ABI0), and Go's compiler calls it so,
  // as it calls `runtime.madvise` (`mov %rax,(%rsp); ...; call madvise`).
  // Read as System V, the call passed its words after six register
  // positions, and RAX came back as a result.
  constexpr va_t Add = kTextVA + 0x100;
  constexpr va_t Caller = kTextVA + 0x200;
  for (const bool Restricted : {false, true}) {
    SCOPED_TRACE(Restricted);
    GoTestImage T;
    T.writeText(Add, {0x48, 0x8B, 0x44, 0x24, 0x08, 0x48, 0x03, 0x44, 0x24,
                      0x10, 0x48, 0x89, 0x44, 0x24, 0x18, 0xC3});
    std::vector<uint8_t> Body = {0x48, 0x83, 0xEC, 0x20, 0x48, 0xC7, 0x04,
                                 0x24, 0x01, 0x00, 0x00, 0x00, 0x48, 0xC7,
                                 0x44, 0x24, 0x08, 0x02, 0x00, 0x00, 0x00};
    const std::vector<uint8_t> Call = makeCall(Caller + Body.size(), Add);
    Body.insert(Body.end(), Call.begin(), Call.end());
    Body.insert(Body.end(),
                {0x48, 0x8B, 0x44, 0x24, 0x10, 0x48, 0x83, 0xC4, 0x20, 0xC3});
    T.writeText(Caller, Body);
    GoModuleInfo Module;
    Module.PclnTabMagic = 0xFFFFFFF1u;
    Module.RegisterArguments = true;
    Module.GoCode = {{Add, kTextVA + 0x300}};
    Module.GoAssembly = {{Add, Add + 0x10}};
    Module.FrameArguments = {{Add, 24}, {Caller, 0}};
    T.Img.ExceptionMetadata.GoModule = std::move(Module);
    for (const auto &[Entry, Name] :
         {std::pair{Add, "main.add"}, std::pair{Caller, "main.f"}}) {
      Symbol Function = Symbol::makeFunc(Entry);
      Function.Name = Name;
      T.Img.Symbols.push_back(std::move(Function));
    }
    EXPECT_TRUE(goTakesStackArguments(T.Img, Add));
    EXPECT_FALSE(goTakesStackArguments(T.Img, Caller));

    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    if (Restricted)
      Options.OnlyFunctionEntries = {Caller};
    llvm::LLVMContext Context;
    const PipelineResult R = Pipeline().run(T.Img, Context, Options);
    ASSERT_TRUE(R.Success) << R.Error;
    const auto F = llvm::find_if(
        R.MedFuncs, [&](const MedFunc &M) { return M.Entry == Caller; });
    ASSERT_NE(F, R.MedFuncs.end());
    ASSERT_EQ(F->CallInfos.size(), 1u);
    const std::vector<MedVar> &Arguments = F->CallInfos[0].Args;
    ASSERT_EQ(Arguments.size(), 2u);
    EXPECT_TRUE(Arguments[0].isConst() && Arguments[0].ConstVal == 1);
    EXPECT_TRUE(Arguments[1].isConst() && Arguments[1].ConstVal == 2);
    if (Restricted)
      continue;
    const auto A = llvm::find_if(
        R.MedFuncs, [&](const MedFunc &M) { return M.Entry == Add; });
    ASSERT_NE(A, R.MedFuncs.end());
    EXPECT_TRUE(A->StackArgumentsOnly);
    EXPECT_EQ(A->Params.size(), 2u);
  }
}

/// The parameters of the function at \p Entry in \p R, by register.
std::vector<uint64_t> parameterRegisters(const PipelineResult &R, va_t Entry) {
  std::vector<uint64_t> Registers;
  for (const MedFunc &M : R.MedFuncs)
    if (M.Entry == Entry)
      for (const MedVar &Parameter : M.Params)
        Registers.push_back(Parameter.RegOff);
  return Registers;
}

/// The constant arguments of the only call the function at \p Entry in \p R
/// makes, ~0 for any other operand, or none where it makes another number of
/// calls.
std::vector<uint64_t> constantArguments(const PipelineResult &R, va_t Entry) {
  std::vector<uint64_t> Constants;
  const MedOp *Call = nullptr;
  for (const MedFunc &M : R.MedFuncs)
    if (M.Entry == Entry)
      for (const MedBlock &Block : M.Blocks)
        for (const MedOp &Op : Block.Ops)
          if (Op.Opcode == NdOp::CALL) {
            if (Call)
              return {};
            Call = &Op;
          }
  if (!Call)
    return {};
  for (unsigned I = 1; I < Call->NumInputs; ++I)
    Constants.push_back(Call->Inputs[I].isConst() ? Call->Inputs[I].ConstVal
                                                  : ~0ull);
  return Constants;
}

TEST(GoAssembly, AHelperTakesTheRegistersItReads) {
  for (const bool StackABI : {false, true}) {
    SCOPED_TRACE(StackABI);
    // Go assembly with no Go declaration of its own takes its operands where
    // its callers set them, as internal/bytealg's indexbytebody<> takes SI,
    // BX, AL and R8.  System V's positions made them RDI, RSI, RDX, RCX and
    // R8, and left the RAX and RBX it reads undefined.
    //   main.f: mov esi, 1; mov ebx, 2; mov eax, 3; mov r8d, 4; call h; ret
    //   h:      lea rax, [rax+rbx]; add rax, rsi; mov [r8], rax; ret
    constexpr va_t F = kTextVA + 0x100;
    constexpr va_t H = kTextVA + 0x200;
    GoTestImage T;
    std::vector<uint8_t> Body = {0xBE, 1, 0, 0, 0,    0xBB, 2, 0, 0, 0, 0xB8,
                                 3,    0, 0, 0, 0x41, 0xB8, 4, 0, 0, 0};
    const std::vector<uint8_t> Call = makeCall(F + Body.size(), H);
    Body.insert(Body.end(), Call.begin(), Call.end());
    Body.push_back(0xC3);
    T.writeText(F, Body);
    T.writeText(
        H, {0x48, 0x8D, 0x04, 0x18, 0x48, 0x01, 0xF0, 0x49, 0x89, 0x00, 0xC3});
    for (const auto &[Entry, Name] :
         {std::pair{F, "main.f"}, std::pair{H, "main.h"}}) {
      Symbol Function = Symbol::makeFunc(Entry);
      Function.Name = Name;
      T.Img.Symbols.push_back(std::move(Function));
    }
    GoModuleInfo Module;
    Module.PclnTabMagic = 0xFFFFFFF1u;
    // Go 1.16 and earlier pass every argument of Go code on the stack and mark
    // no function as assembly; the helper's record gives no size.
    Module.RegisterArguments = !StackABI;
    Module.GoCode = {{F, kTextVA + 0x300}};
    if (!StackABI)
      Module.GoAssembly = {{H, H + 0x10}};
    // Go 1.16's `cmpbody<>` declares `$0-0`, a record of no argument word.
    Module.FrameArguments = {
        {F, 0, 0},
        {H, StackABI ? std::optional<uint32_t>(0) : std::nullopt,
         std::nullopt}};
    T.Img.ExceptionMetadata.GoModule = std::move(Module);
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    llvm::LLVMContext Context;
    const PipelineResult R = Pipeline().run(T.Img, Context, Options);
    ASSERT_TRUE(R.Success) << R.Error;
    // In the platform's order, then Go's.
    using namespace x86reg;
    EXPECT_EQ(parameterRegisters(R, H),
              (std::vector<uint64_t>{RSI, R8, RAX, RBX}));
    EXPECT_EQ(constantArguments(R, F), (std::vector<uint64_t>{1, 4, 3, 2}));
  }
}

TEST(GoAssembly, AVectorRegisterKeptInPlaceOnOneWayIsNoArgument) {
  // An assembly helper shaped as memeqbody<>: it compares 16 bytes at SI
  // and DI with SSE where BX says there are as many, and only clears the
  // vector registers' upper lanes with `vzeroupper` where there are fewer.
  // X0 at its return joins a value it computed and the register as its
  // caller left it, which hands the caller back nothing new: X0 and X1 are
  // no arguments, as they were, and the call passed two unknown values.
  //   main.f: mov esi, 1; mov edi, 2; mov ebx, 3; call h; ret
  //   h:      cmp rbx, 16; jb small; movdqu xmm0, [rsi]; movdqu xmm1, [rdi]
  //           pcmpeqb xmm0, xmm1; pmovmskb eax, xmm0; cmp eax, 0xffff
  //           sete al; jmp done
  //   small:  vzeroupper; xor eax, eax
  //   done:   ret
  constexpr va_t F = kTextVA + 0x100;
  constexpr va_t H = kTextVA + 0x200;
  GoTestImage T;
  std::vector<uint8_t> Body = {0xBE, 1, 0,    0, 0, 0xBF, 2, 0,
                               0,    0, 0xBB, 3, 0, 0,    0};
  const std::vector<uint8_t> Call = makeCall(F + Body.size(), H);
  Body.insert(Body.end(), Call.begin(), Call.end());
  Body.push_back(0xC3);
  T.writeText(F, Body);
  T.writeText(H, {0x48, 0x83, 0xFB, 0x10, 0x72, 0x1A, 0xF3, 0x0F, 0x6F, 0x06,
                  0xF3, 0x0F, 0x6F, 0x0F, 0x66, 0x0F, 0x74, 0xC1, 0x66, 0x0F,
                  0xD7, 0xC0, 0x3D, 0xFF, 0xFF, 0x00, 0x00, 0x0F, 0x94, 0xC0,
                  0xEB, 0x05, 0xC5, 0xF8, 0x77, 0x31, 0xC0, 0xC3});
  for (const auto &[Entry, Name] :
       {std::pair{F, "main.f"}, std::pair{H, "main.h"}}) {
    Symbol Function = Symbol::makeFunc(Entry);
    Function.Name = Name;
    T.Img.Symbols.push_back(std::move(Function));
  }
  GoModuleInfo Module;
  Module.PclnTabMagic = 0xFFFFFFF1u;
  Module.RegisterArguments = true;
  Module.GoCode = {{F, kTextVA + 0x300}};
  Module.GoAssembly = {{H, H + 0x30}};
  Module.FrameArguments = {{F, 0, 0}, {H, std::nullopt, std::nullopt}};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  llvm::LLVMContext Context;
  const PipelineResult R = Pipeline().run(T.Img, Context, Options);
  ASSERT_TRUE(R.Success) << R.Error;
  using namespace x86reg;
  // In the platform's order, then Go's.
  EXPECT_EQ(parameterRegisters(R, H), (std::vector<uint64_t>{RDI, RSI, RBX}));
  EXPECT_EQ(constantArguments(R, F), (std::vector<uint64_t>{2, 1, 3}));
}

TEST(GoRegisterArguments, RegistersStoredInARowAreNoVariadicSaveArea) {
  // System V reads RAX before a write as a variadic prologue's vector count,
  // and the argument registers it then stores into consecutive words, from
  // some position through R9, as the save area, which a caller fills only
  // as far as it passes arguments.  In Go, RAX is the first argument:
  // `runtime.spillArgs` stores R8 and R9 in a row, and took neither.
  //   main.g: mov [rdi], rax; mov [rdi+8], r8; mov [rdi+16], r9; ret
  //   main.f: mov eax, 1; ... mov r9d, 7; call main.g; ret
  constexpr va_t F = kTextVA + 0x100;
  constexpr va_t G = kTextVA + 0x200;
  GoTestImage T;
  std::vector<uint8_t> Body;
  const uint8_t Moves[][2] = {{0, 0xB8}, {0, 0xBB},    {0, 0xB9},   {0, 0xBF},
                              {0, 0xBE}, {0x41, 0xB8}, {0x41, 0xB9}};
  uint8_t Value = 1;
  for (const auto &Move : Moves) {
    if (Move[0])
      Body.push_back(Move[0]);
    Body.insert(Body.end(), {Move[1], Value++, 0, 0, 0});
  }
  const std::vector<uint8_t> Call = makeCall(F + Body.size(), G);
  Body.insert(Body.end(), Call.begin(), Call.end());
  Body.push_back(0xC3);
  T.writeText(F, Body);
  T.writeText(G, {0x48, 0x89, 0x07, 0x4C, 0x89, 0x47, 0x08, 0x4C, 0x89, 0x4F,
                  0x10, 0xC3});
  for (const auto &[Entry, Name] :
       {std::pair{F, "main.f"}, std::pair{G, "main.g"}}) {
    Symbol Function = Symbol::makeFunc(Entry);
    Function.Name = Name;
    T.Img.Symbols.push_back(std::move(Function));
  }
  GoModuleInfo Module;
  Module.PclnTabMagic = 0xFFFFFFF1u;
  Module.RegisterArguments = true;
  Module.GoCode = {{F, kTextVA + 0x300}};
  Module.FrameArguments = {{F, 0, 0}, {G, 56, 7}};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  llvm::LLVMContext Context;
  const PipelineResult R = Pipeline().run(T.Img, Context, Options);
  ASSERT_TRUE(R.Success) << R.Error;
  using namespace x86reg;
  EXPECT_EQ(parameterRegisters(R, G),
            (std::vector<uint64_t>{RAX, RBX, RCX, RDI, RSI, R8, R9}));
  // The call passes all seven; the ones main.g never reads are zero.
  const std::vector<uint64_t> Arguments = constantArguments(R, F);
  ASSERT_EQ(Arguments.size(), 7u);
  EXPECT_EQ(Arguments[0], 1u);
  EXPECT_EQ(Arguments[3], 4u);
  EXPECT_EQ(Arguments[5], 6u);
  EXPECT_EQ(Arguments[6], 7u);
}

TEST(GoAssembly, AnARM64ForwarderIsNoABI0Assembly) {
  // An arm64 call takes its arguments from the setup its caller writes, so
  // no operation of a routine reads the registers it passes on in a call.
  // `runtime.memhash` declared <ABIInternal> tail-calls `aeshashbody` with
  // X0-X2 and reads no word of its caller's frame; ABI0 assembly does.
  //   main.add: ldr x0, [sp, #8]; ldr x1, [sp, #16]; add x0, x0, x1
  //             str x0, [sp, #24]; ret
  //   main.fwd: b main.other
  constexpr va_t Add = kTextVA + 0x100;
  constexpr va_t Forwarder = kTextVA + 0x140;
  constexpr va_t Other = kTextVA + 0x180;
  GoTestImage T(Arch::AArch64);
  T.writeText(
      Add, words({0xF94007E0, 0xF9400BE1, 0x8B010000, 0xF9000FE0, 0xD65F03C0}));
  T.writeText(Forwarder,
              words({0x14000000u | (((Other - Forwarder) >> 2) & 0x3FFFFFFu)}));
  T.writeText(Other, words({0x8B010000, 0xD65F03C0}));
  GoModuleInfo Module;
  Module.RegisterArguments = true;
  Module.GoCode = {{Add, kTextVA + 0x200}};
  Module.GoAssembly = {{Add, Other}};
  Module.FrameArguments = {{Add, 24}, {Forwarder, 24}, {Other, 24}};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  for (const auto &[Entry, Name] :
       {std::pair{Add, "main.add"}, std::pair{Forwarder, "main.fwd"},
        std::pair{Other, "main.other"}}) {
    Symbol Function = Symbol::makeFunc(Entry);
    Function.Name = Name;
    T.Img.Symbols.push_back(std::move(Function));
  }
  const MedFunc Forwarding = lift(T.Img, Forwarder);
  // The branch leaves for main.other, whose body is no part of main.fwd.
  bool ReadsX1 = false;
  for (const MedBlock &Block : Forwarding.Blocks)
    for (const MedOp &Op : Block.Ops)
      for (unsigned I = 0; I < Op.NumInputs; ++I)
        ReadsX1 |= Op.Opcode == NdOp::INT_ADD &&
                   Op.Inputs[I].Kind == MedVar::Reg &&
                   Op.Inputs[I].RegOff == a64reg::X1;
  ASSERT_FALSE(ReadsX1);
  EXPECT_FALSE(Forwarding.StackArgumentsOnly);
  const MedFunc AddFunc = lift(T.Img, Add);
  EXPECT_TRUE(AddFunc.StackArgumentsOnly);
  EXPECT_EQ(AddFunc.Params.size(), 2u);
}

TEST(GoInternalABI, AnIndirectCallReturnsEveryResultItsCallerReads) {
  // Go's internal ABI returns results in RAX, RBX, RCX, RDI, RSI, R8-R11 and
  // keeps none of those registers but its results, so a read of RBX after a
  // call is its second result.  Modelled with RAX alone, an interface's data
  // word (`call [rdx]; mov [rsp+8], rbx`) printed a placeholder.
  //   main.f: sub rsp, 0x18; mov rdx, [rip+0x2000]; call [rdx]
  //           mov [rsp+8], <reg>; add rsp, 0x18; ret
  for (const uint8_t ModRM : {uint8_t(0x5C), uint8_t(0x54)}) {
    const bool ReadsRBX = ModRM == 0x5C; // 0x54 stores RDX, no result
    SCOPED_TRACE(ReadsRBX);
    constexpr va_t Caller = kTextVA + 0x100;
    GoTestImage T;
    T.writeText(Caller, {0x48, 0x83, 0xEC, 0x18, 0x48, 0x8B, 0x15, 0x00,
                         0x20, 0x00, 0x00, 0xFF, 0x12, 0x48, 0x89, ModRM,
                         0x24, 0x08, 0x48, 0x83, 0xC4, 0x18, 0xC3});
    GoModuleInfo Module;
    Module.PclnTabMagic = 0xFFFFFFF1u;
    Module.RegisterArguments = true;
    Module.GoCode = {{Caller, kTextVA + 0x200}};
    T.Img.ExceptionMetadata.GoModule = std::move(Module);
    const MedFunc F = lift(T.Img, Caller);
    const MedOp *Call = onlyCall(F);
    ASSERT_NE(Call, nullptr);
    ASSERT_EQ(Call->Opcode, NdOp::INDIR_CALL);
    EXPECT_EQ(Call->RegisterContract, CallRegisterContract::GoInternal);
    if (!ReadsRBX) {
      EXPECT_FALSE(Call->RegisterReturnFields);
      continue;
    }
    ASSERT_TRUE(Call->RegisterReturnFields);
    ASSERT_EQ(Call->RegisterReturnFields->size(), 2u);
    EXPECT_EQ((*Call->RegisterReturnFields)[0].RegOff, x86reg::RAX);
    EXPECT_EQ((*Call->RegisterReturnFields)[1].RegOff, x86reg::RBX);
    EXPECT_EQ(Call->Output.Kind, MedVar::Temp);
    EXPECT_EQ(Call->Output.Size, 16u);
    for (const MedCallClobber &Clobber : F.CallClobbers)
      EXPECT_FALSE(Clobber.CallSiteId == Call->CallSiteId &&
                   Clobber.Value.Kind == MedVar::Reg &&
                   Clobber.Value.RegOff == x86reg::RBX);
    // Every later read of RBX is the call's second field.
    const MedOp *Field = nullptr;
    for (const MedBlock &Block : F.Blocks)
      for (const MedOp &Op : Block.Ops)
        if (Op.Opcode == NdOp::SUBBYTES && Op.NumInputs == 2 &&
            Op.Inputs[0].Kind == MedVar::Temp &&
            Op.Inputs[0].Id == Call->Output.Id && Op.Inputs[1].isConst() &&
            Op.Inputs[1].ConstVal == 8)
          Field = &Op;
    ASSERT_NE(Field, nullptr);
    unsigned Reads = 0;
    for (const MedBlock &Block : F.Blocks)
      for (const MedOp &Op : Block.Ops)
        for (unsigned I = 0; I < Op.NumInputs; ++I)
          if (Op.Addr > Call->Addr && Op.Inputs[I].Kind == MedVar::Reg &&
              Op.Inputs[I].RegOff == x86reg::RBX) {
            EXPECT_EQ(Op.Inputs[I].Id, Field->Output.Id);
            EXPECT_EQ(Op.Inputs[I].SSAVer, Field->Output.SSAVer);
            ++Reads;
          }
    EXPECT_GT(Reads, 0u);
  }
}

TEST(GoInternalABI, AByteWriteKeepsNoResultOfTheCall) {
  // An 8-bit write keeps the register's upper bytes, which the lift slices
  // off the old value and joins above the new byte: after a call, those are
  // bytes the call left, no result the caller reads.  Read as one, they gave
  // the call eight results, and every function that returns its call's
  // results the same (`runtime.preemptone`, a bool, returned eight words).
  //   main.f: sub rsp, 0x18; mov rdx, [rip+0x2000]; call [rdx]
  //           mov r10b, 1; mov rax, r10; add rsp, 0x18; ret
  constexpr va_t Caller = kTextVA + 0x100;
  GoTestImage T;
  T.writeText(Caller, {0x48, 0x83, 0xEC, 0x18, 0x48, 0x8B, 0x15, 0x00,
                       0x20, 0x00, 0x00, 0xFF, 0x12, 0x41, 0xB2, 0x01,
                       0x4C, 0x89, 0xD0, 0x48, 0x83, 0xC4, 0x18, 0xC3});
  GoModuleInfo Module;
  Module.PclnTabMagic = 0xFFFFFFF1u;
  Module.RegisterArguments = true;
  Module.GoCode = {{Caller, kTextVA + 0x200}};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  const MedFunc F = lift(T.Img, Caller);
  const MedOp *Call = onlyCall(F);
  ASSERT_NE(Call, nullptr);
  EXPECT_FALSE(Call->RegisterReturnFields);
}

TEST(GoInternalABI, EveryDirectCallOfAFunctionReturnsTheSameResults) {
  // A Go function returns one record to every call of it, as many results as
  // any caller reads; a function whose return hands back what a call it
  // makes returns reads those results of that call.  The results are loads,
  // which no summary of the callee replaces with a constant at its calls.
  //   main.f: mov rbx, [rax+8]; mov rax, [rax]; ret
  //   main.w: sub rsp, 8; call main.f; add rsp, 8; ret
  //   main.g: sub rsp, 0x18; call main.w; mov [rsp+8], rbx
  //           mov rax, [rsp+8]; add rsp, 0x18; ret
  //   main.h: sub rsp, 8; call main.f; add rsp, 8; ret
  constexpr va_t F = kTextVA + 0x100;
  constexpr va_t W = kTextVA + 0x140;
  constexpr va_t G = kTextVA + 0x180;
  constexpr va_t H = kTextVA + 0x1C0;
  GoTestImage T;
  T.writeText(F, {0x48, 0x8B, 0x58, 0x08, 0x48, 0x8B, 0x00, 0xC3});
  const auto Body = [&](va_t At, va_t Callee, std::vector<uint8_t> After,
                        uint8_t Frame) {
    std::vector<uint8_t> Code = {0x48, 0x83, 0xEC, Frame};
    const std::vector<uint8_t> Call = makeCall(At + Code.size(), Callee);
    Code.insert(Code.end(), Call.begin(), Call.end());
    Code.insert(Code.end(), After.begin(), After.end());
    Code.insert(Code.end(), {0x48, 0x83, 0xC4, Frame, 0xC3});
    T.writeText(At, Code);
  };
  Body(W, F, {}, 0x08);
  Body(G, W, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x8B, 0x44, 0x24, 0x08},
       0x18);
  Body(H, F, {}, 0x08);
  GoModuleInfo Module;
  Module.PclnTabMagic = 0xFFFFFFF1u;
  Module.RegisterArguments = true;
  Module.GoCode = {{F, kTextVA + 0x300}};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  for (const auto &[Entry, Name] :
       {std::pair{F, "main.f"}, std::pair{W, "main.w"}, std::pair{G, "main.g"},
        std::pair{H, "main.h"}}) {
    Symbol Function = Symbol::makeFunc(Entry);
    Function.Name = Name;
    T.Img.Symbols.push_back(std::move(Function));
  }
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  llvm::LLVMContext Context;
  const PipelineResult R = Pipeline().run(T.Img, Context, Options);
  ASSERT_TRUE(R.Success) << R.Error;
  const auto Function = [&](va_t Entry) -> const MedFunc & {
    const auto It = llvm::find_if(
        R.MedFuncs, [&](const MedFunc &M) { return M.Entry == Entry; });
    EXPECT_NE(It, R.MedFuncs.end());
    return *It;
  };
  const auto ReturnsRAXAndRBX = [](const std::vector<MedReturnReg> &Shape) {
    return Shape.size() == 2 && Shape[0].RegOff == x86reg::RAX &&
           Shape[1].RegOff == x86reg::RBX;
  };
  EXPECT_TRUE(ReturnsRAXAndRBX(Function(F).MultiReturn));
  EXPECT_TRUE(ReturnsRAXAndRBX(Function(W).MultiReturn));
  // Only the callers tell that RBX is a result, after the function's dead
  // code is gone: the write that defines it at the return stays.
  EXPECT_TRUE(llvm::any_of(Function(F).Blocks, [](const MedBlock &Block) {
    return llvm::any_of(Block.Ops, [](const MedOp &Op) {
      return Op.Output.Kind == MedVar::Reg && Op.Output.RegOff == x86reg::RBX;
    });
  }));
  EXPECT_TRUE(Function(G).MultiReturn.empty());
  for (const va_t Caller : {G, W, H}) {
    SCOPED_TRACE(Caller);
    const MedOp *Call = onlyCall(Function(Caller));
    ASSERT_NE(Call, nullptr);
    ASSERT_TRUE(Call->RegisterReturnFields);
    EXPECT_TRUE(ReturnsRAXAndRBX(*Call->RegisterReturnFields));
  }
}

/// A Go image of the functions \p Code holds, each named and in Go code, at
/// its address: an image whose compiled code takes Go's register arguments.
GoTestImage goFunctions(
    const std::vector<
        std::pair<std::pair<va_t, const char *>, std::vector<uint8_t>>> &Code) {
  GoTestImage T;
  for (const auto &[Function, Bytes] : Code) {
    T.writeText(Function.first, Bytes);
    Symbol Entry = Symbol::makeFunc(Function.first);
    Entry.Name = Function.second;
    T.Img.Symbols.push_back(std::move(Entry));
  }
  GoModuleInfo Module;
  Module.PclnTabMagic = 0xFFFFFFF1u;
  Module.RegisterArguments = true;
  Module.GoCode = {{kTextVA, kTextVA + 0x400}};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  return T;
}

/// A caller that reads RBX after it calls \p Callee and returns it.
///   sub rsp, 0x18; call Callee; mov [rsp+8], rbx; mov rax, [rsp+8]
///   add rsp, 0x18; ret
std::vector<uint8_t> readsRBXAfter(va_t At, va_t Callee) {
  std::vector<uint8_t> Code = {0x48, 0x83, 0xEC, 0x18};
  const std::vector<uint8_t> Call = makeCall(At + Code.size(), Callee);
  Code.insert(Code.end(), Call.begin(), Call.end());
  Code.insert(Code.end(), {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x8B, 0x44, 0x24,
                           0x08, 0x48, 0x83, 0xC4, 0x18, 0xC3});
  return Code;
}

TEST(GoInternalABI, AFunctionReturnsNoResultAReturnLeavesUndefined) {
  // Every return of a function hands back each result it has.  One that
  // leaves RBX as the function entered with it, where no argument arrives,
  // returns no second result, whatever a caller reads there: the caller's
  // read of the register shows that, not every call of the function.
  //   main.f: mov rcx, rax; xor eax, eax; test rcx, rcx; je 1f
  //           mov rbx, [rcx]; 1: ret
  constexpr va_t F = kTextVA + 0x100;
  constexpr va_t G = kTextVA + 0x180;
  GoTestImage T = goFunctions({{{F, "main.f"},
                                {0x48, 0x89, 0xC1, 0x31, 0xC0, 0x48, 0x85, 0xC9,
                                 0x74, 0x03, 0x48, 0x8B, 0x19, 0xC3}},
                               {{G, "main.g"}, readsRBXAfter(G, F)}});
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  llvm::LLVMContext Context;
  const PipelineResult R = Pipeline().run(T.Img, Context, Options);
  ASSERT_TRUE(R.Success) << R.Error;
  const auto Function = [&](va_t Entry) -> const MedFunc & {
    const auto It = llvm::find_if(
        R.MedFuncs, [&](const MedFunc &M) { return M.Entry == Entry; });
    EXPECT_NE(It, R.MedFuncs.end());
    return *It;
  };
  EXPECT_TRUE(Function(F).MultiReturn.empty());
  const MedOp *Call = onlyCall(Function(G));
  ASSERT_NE(Call, nullptr);
  EXPECT_FALSE(Call->RegisterReturnFields);
}

TEST(GoInternalABI, AByteWriteOnlyItsByteIsReadOfKeepsNoResult) {
  // `setg r9b` merges the byte into what R9 held, and `movzx r9d, r9b` takes
  // only that byte back before anything reads R9 whole, so copy propagation
  // drops the merge's copy into R9.  The merge still read the R9 a call left
  // there, and gave the call seven results: `runtime.traceStack` raised
  // `callers`, and through the functions that return their calls' results
  // `tracebackPCs`, `(*unwinder).next` and `pcvalue` too.
  //   main.f: mov rbx, [rax+8]; mov rcx, [rax+16]; mov rdi, [rax+24]
  //           mov rsi, [rax+32]; mov r8, [rax+40]; mov r9, [rax+48]
  //           mov rax, [rax]; ret
  //   main.g: sub rsp, 8; call main.f; test rax, rax; setg r9b
  //           movzx r9d, r9b; mov rax, r9; add rsp, 8; ret
  constexpr va_t F = kTextVA + 0x100;
  constexpr va_t G = kTextVA + 0x180;
  std::vector<uint8_t> Caller = {0x48, 0x83, 0xEC, 0x08};
  const std::vector<uint8_t> Call = makeCall(G + Caller.size(), F);
  Caller.insert(Caller.end(), Call.begin(), Call.end());
  Caller.insert(Caller.end(),
                {0x48, 0x85, 0xC0, 0x41, 0x0F, 0x9F, 0xC1, 0x45, 0x0F, 0xB6,
                 0xC9, 0x4C, 0x89, 0xC8, 0x48, 0x83, 0xC4, 0x08, 0xC3});
  GoTestImage T =
      goFunctions({{{F, "main.f"},
                    {0x48, 0x8B, 0x58, 0x08, 0x48, 0x8B, 0x48, 0x10, 0x48, 0x8B,
                     0x78, 0x18, 0x48, 0x8B, 0x70, 0x20, 0x4C, 0x8B, 0x40, 0x28,
                     0x4C, 0x8B, 0x48, 0x30, 0x48, 0x8B, 0x00, 0xC3}},
                   {{G, "main.g"}, Caller}});
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  llvm::LLVMContext Context;
  const PipelineResult R = Pipeline().run(T.Img, Context, Options);
  ASSERT_TRUE(R.Success) << R.Error;
  const auto Function = [&](va_t Entry) -> const MedFunc & {
    const auto It = llvm::find_if(
        R.MedFuncs, [&](const MedFunc &M) { return M.Entry == Entry; });
    EXPECT_NE(It, R.MedFuncs.end());
    return *It;
  };
  EXPECT_TRUE(Function(F).MultiReturn.empty());
  const MedOp *Read = onlyCall(Function(G));
  ASSERT_NE(Read, nullptr);
  EXPECT_FALSE(Read->RegisterReturnFields);
}

TEST(GoInternalABI, AResultIsAsWideAsItsCallersReadIt) {
  // Go returns a bool in a result register's low byte, and `setg bl` merges
  // it into upper bytes the function never set.  Its callers read BL alone,
  // so the result is one byte: a word-wide field returned those unknown
  // bytes, a trap on every call.
  //   main.f: mov rcx, rax; mov rax, [rcx]; cmp rcx, 0x10; setg bl; ret
  //   main.g: sub rsp, 8; call main.f; movzx eax, bl; add rsp, 8; ret
  constexpr va_t F = kTextVA + 0x100;
  constexpr va_t G = kTextVA + 0x180;
  std::vector<uint8_t> Caller = {0x48, 0x83, 0xEC, 0x08};
  const std::vector<uint8_t> Call = makeCall(G + Caller.size(), F);
  Caller.insert(Caller.end(), Call.begin(), Call.end());
  Caller.insert(Caller.end(), {0x0F, 0xB6, 0xC3, 0x48, 0x83, 0xC4, 0x08, 0xC3});
  GoTestImage T = goFunctions({{{F, "main.f"},
                                {0x48, 0x89, 0xC1, 0x48, 0x8B, 0x01, 0x48, 0x83,
                                 0xF9, 0x10, 0x0F, 0x9F, 0xC3, 0xC3}},
                               {{G, "main.g"}, Caller}});
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  llvm::LLVMContext Context;
  const PipelineResult R = Pipeline().run(T.Img, Context, Options);
  ASSERT_TRUE(R.Success) << R.Error;
  const auto Function = [&](va_t Entry) -> const MedFunc & {
    const auto It = llvm::find_if(
        R.MedFuncs, [&](const MedFunc &M) { return M.Entry == Entry; });
    EXPECT_NE(It, R.MedFuncs.end());
    return *It;
  };
  const auto ABoolAfterAWord = [](const std::vector<MedReturnReg> &Shape) {
    return Shape.size() == 2 && Shape[0].RegOff == x86reg::RAX &&
           Shape[0].Size == 8 && Shape[1].RegOff == x86reg::RBX &&
           Shape[1].Size == 1;
  };
  EXPECT_TRUE(ABoolAfterAWord(Function(F).MultiReturn));
  const MedOp *Read = onlyCall(Function(G));
  ASSERT_NE(Read, nullptr);
  ASSERT_TRUE(Read->RegisterReturnFields);
  EXPECT_TRUE(ABoolAfterAWord(*Read->RegisterReturnFields));
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions C;
  C.TheArch = T.Img.Arch;
  C.Format = T.Img.Format;
  C.Image = &T.Img;
  ASSERT_TRUE(HighCEmitter().emit(R.HighFuncs, OS, C));
  OS.flush();
  size_t At = 0;
  while ((At = Source.find(" main_f(", At)) != std::string::npos &&
         Source.compare(Source.find('\n', At) - 2, 2, " {") != 0)
    ++At;
  ASSERT_NE(At, std::string::npos) << Source;
  const std::string Body = Source.substr(At, Source.find("\n}", At) - At);
  EXPECT_EQ(Body.find("__builtin_trap"), std::string::npos) << Body;
}

/// The C the pipeline \p R gives the function \p Name of \p Img: its body, from
/// its signature on.
std::string highCBody(const PipelineResult &R, const BinaryImage &Img,
                      const std::string &Name) {
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions C;
  C.TheArch = Img.Arch;
  C.Format = Img.Format;
  C.Image = &Img;
  if (!HighCEmitter().emit(R.HighFuncs, OS, C))
    return {};
  OS.flush();
  size_t At = 0;
  while ((At = Source.find(" " + Name + "(", At)) != std::string::npos &&
         Source.compare(Source.find('\n', At) - 2, 2, " {") != 0)
    ++At;
  if (At == std::string::npos)
    return {};
  return Source.substr(At, Source.find("\n}", At) - At);
}

/// How many reads of the current goroutine the function at \p Entry in \p R
/// makes, and whether it reads a version of the goroutine register that no
/// operation defines: the incoming value, or what a call left there.
std::pair<unsigned, bool> goroutineReads(const PipelineResult &R, va_t Entry,
                                         uint64_t Register) {
  unsigned Reads = 0;
  bool Undefined = false;
  for (const MedFunc &M : R.MedFuncs) {
    if (M.Entry != Entry)
      continue;
    std::set<std::pair<int, int>> Defined;
    for (const MedBlock &Block : M.Blocks) {
      for (const PhiNode &Phi : Block.Phis)
        Defined.insert({Phi.Output.Id, Phi.Output.SSAVer});
      for (const MedOp &Op : Block.Ops)
        Defined.insert({Op.Output.Id, Op.Output.SSAVer});
    }
    for (const MedBlock &Block : M.Blocks)
      for (const MedOp &Op : Block.Ops) {
        if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs >= 1 &&
            Op.Inputs[0].isConst() &&
            Op.Inputs[0].ConstVal ==
                static_cast<uint64_t>(Intrinsic::GoCurrentGoroutine))
          ++Reads;
        for (unsigned I = 0; I < Op.NumInputs; ++I)
          Undefined |= Op.Inputs[I].Kind == MedVar::Reg &&
                       Op.Inputs[I].RegOff == Register &&
                       !Defined.count({Op.Inputs[I].Id, Op.Inputs[I].SSAVer});
      }
  }
  return {Reads, Undefined};
}

TEST(GoInternalABI, TheGoroutineRegisterHoldsTheCurrentGoroutine) {
  // Go's internal ABI keeps the current goroutine in R14, which compiled
  // code never writes and reads as getg(): at the entry, and after a call,
  // which may have changed it (`needm` installs another).  It had no
  // definition, so `runtime.canpanic` read every `getg().m` as a trap.
  //   main.f: mov rcx, [r14+0x30]; call main.g; mov rax, [r14+0x30];
  //           add rax, rcx; ret
  //   main.g: ret
  constexpr va_t F = kTextVA + 0x100;
  constexpr va_t G = kTextVA + 0x200;
  GoTestImage T;
  std::vector<uint8_t> Body = {0x49, 0x8B, 0x4E, 0x30};
  const std::vector<uint8_t> Call = makeCall(F + Body.size(), G);
  Body.insert(Body.end(), Call.begin(), Call.end());
  Body.insert(Body.end(), {0x49, 0x8B, 0x46, 0x30, 0x48, 0x01, 0xC8, 0xC3});
  T.writeText(F, Body);
  T.writeText(G, {0xC3});
  for (const auto &[Entry, Name] :
       {std::pair{F, "main.f"}, std::pair{G, "main.g"}}) {
    Symbol Function = Symbol::makeFunc(Entry);
    Function.Name = Name;
    T.Img.Symbols.push_back(std::move(Function));
  }
  GoModuleInfo Module;
  Module.PclnTabMagic = 0xFFFFFFF1u;
  Module.RegisterArguments = true;
  Module.GoCode = {{F, kTextVA + 0x300}};
  Module.FrameArguments = {{F, 0, 0}, {G, 0, 0}};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  llvm::LLVMContext Context;
  const PipelineResult R = Pipeline().run(T.Img, Context, Options);
  ASSERT_TRUE(R.Success) << R.Error;
  // main.g writes no register, so the call keeps R14 and the read at the
  // entry serves both loads.
  EXPECT_EQ(goroutineReads(R, F, x86reg::R14),
            (std::pair<unsigned, bool>{1, false}));
  EXPECT_TRUE(parameterRegisters(R, F).empty());
  const std::string Text = highCBody(R, T.Img, "main_f");
  ASSERT_FALSE(Text.empty());
  EXPECT_EQ(Text.find("__builtin_trap"), std::string::npos) << Text;
  EXPECT_NE(Text.find("%%r14"), std::string::npos) << Text;
}

TEST(GoInternalABI, AnARM64GoroutineRegisterHoldsTheCurrentGoroutine) {
  // arm64 keeps the goroutine in R28, which no summary tracks, so the call
  // may change it: one read at the entry and one after the call.
  //   main.f: ldr x1, [x28, #48]; str x30, [sp, #-32]!; str x1, [sp, #8]
  //           bl main.g; ldr x0, [x28, #48]; ldr x1, [sp, #8]
  //           add x0, x0, x1; ldr x30, [sp], #32; ret
  //   main.g: ret
  constexpr va_t F = kTextVA + 0x100;
  constexpr va_t G = kTextVA + 0x180;
  GoTestImage T(Arch::AArch64);
  const uint32_t Call = 0x94000000u | (((G - (F + 12)) >> 2) & 0x3FFFFFFu);
  T.writeText(F, words({0xF9401B81, 0xF81E0FFE, 0xF90007E1, Call, 0xF9401B80,
                        0xF94007E1, 0x8B010000, 0xF84207FE, 0xD65F03C0}));
  T.writeText(G, words({0xD65F03C0}));
  for (const auto &[Entry, Name] :
       {std::pair{F, "main.f"}, std::pair{G, "main.g"}}) {
    Symbol Function = Symbol::makeFunc(Entry);
    Function.Name = Name;
    T.Img.Symbols.push_back(std::move(Function));
  }
  GoModuleInfo Module;
  Module.PclnTabMagic = 0xFFFFFFF1u;
  Module.RegisterArguments = true;
  Module.GoCode = {{kTextVA, kTextVA + 0x400}};
  Module.FrameArguments = {{F, 0, 0}, {G, 0, 0}};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  llvm::LLVMContext Context;
  const PipelineResult R = Pipeline().run(T.Img, Context, Options);
  ASSERT_TRUE(R.Success) << R.Error;
  EXPECT_EQ(goroutineReads(R, F, a64reg::X28),
            (std::pair<unsigned, bool>{2, false}));
  const std::string Text = highCBody(R, T.Img, "main_f");
  ASSERT_FALSE(Text.empty());
  EXPECT_EQ(Text.find("__builtin_trap"), std::string::npos) << Text;
  EXPECT_NE(Text.find("x28"), std::string::npos) << Text;
}

TEST(GoFunctionTable, CodeInsideARecordedFunctionBeginsNone) {
  // A Go image's pclntab records every function of its Go code, so code
  // inside one begins none.  `runtime.suspendG`'s jump table holds case
  // labels that discovery took for entries, and each `jmp` to one became a
  // call into a function that starts in the middle of suspendG.
  //   main.f: nop x16; ret
  //   main.g: call main.f+8; ret
  constexpr va_t F = kTextVA + 0x100;
  constexpr va_t G = kTextVA + 0x200;
  GoTestImage T;
  std::vector<uint8_t> Body(16, 0x90);
  Body.push_back(0xC3);
  T.writeText(F, Body);
  std::vector<uint8_t> Caller = makeCall(G, F + 8);
  Caller.push_back(0xC3);
  T.writeText(G, Caller);
  for (const auto &[Entry, Name] :
       {std::pair{F, "main.f"}, std::pair{G, "main.g"}}) {
    Symbol Function = Symbol::makeFunc(Entry);
    Function.Name = Name;
    T.Img.Symbols.push_back(std::move(Function));
  }
  GoModuleInfo Module;
  Module.PclnTabMagic = 0xFFFFFFF1u;
  Module.RegisterArguments = true;
  Module.GoCode = {{F, kTextVA + 0x300}};
  Module.FrameArguments = {{F, 0, 0}, {G, 0, 0}};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  Decoder Dec;
  ASSERT_TRUE(Dec.init(T.Img));
  const auto Found = FuncDetector().detect(T.Img, Dec);
  const auto Begins = [&](va_t Entry) {
    return llvm::any_of(Found,
                        [&](const auto &Item) { return Item.first == Entry; });
  };
  EXPECT_TRUE(Begins(F));
  EXPECT_TRUE(Begins(G));
  EXPECT_FALSE(Begins(F + 8));
}

TEST(GoFunctionTable, ACallIntoADuffsDeviceRunsItsBody) {
  // Go's compiler calls `runtime.duffzero` at the point that zeroes as many
  // bytes as it needs, and expects the call to change only the registers that
  // straight-line body writes: the caller keeps RBX across it.  The body runs
  // in the caller's place, and the entry begins no function of its own.
  //   runtime.duffzero: movups [rdi], xmm15; movups [rdi+16], xmm15
  //                     lea rdi, [rdi+32]; movups [rdi], xmm15; ret
  //   main.g:           mov rdi, rax; call runtime.duffzero+4; mov rax, rbx
  //                     ret
  constexpr va_t Z = kTextVA + 0x100;
  constexpr va_t G = kTextVA + 0x200;
  GoTestImage T;
  T.writeText(Z, {0x44, 0x0F, 0x11, 0x3F, 0x44, 0x0F, 0x11, 0x7F, 0x10, 0x48,
                  0x8D, 0x7F, 0x20, 0x44, 0x0F, 0x11, 0x3F, 0xC3});
  std::vector<uint8_t> Caller = {0x48, 0x89, 0xC7};
  const std::vector<uint8_t> Call = makeCall(G + Caller.size(), Z + 4);
  Caller.insert(Caller.end(), Call.begin(), Call.end());
  Caller.insert(Caller.end(), {0x48, 0x89, 0xD8, 0xC3});
  T.writeText(G, Caller);
  for (const auto &[Entry, Name] :
       {std::pair{Z, "runtime.duffzero"}, std::pair{G, "main.g"}}) {
    Symbol Function = Symbol::makeFunc(Entry);
    Function.Name = Name;
    T.Img.Symbols.push_back(std::move(Function));
  }
  GoModuleInfo Module;
  Module.PclnTabMagic = 0xFFFFFFF1u;
  Module.RegisterArguments = true;
  Module.GoCode = {{Z, kTextVA + 0x300}};
  Module.GoAssembly = {{Z, Z + 0x20}};
  Module.FrameArguments = {{Z, 0, 0}, {G, 16, std::nullopt}};
  Module.DuffDevices = {{Z, Z + 0x20}};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  Decoder Dec;
  ASSERT_TRUE(Dec.init(T.Img));
  const auto Found = FuncDetector().detect(T.Img, Dec);
  EXPECT_FALSE(llvm::any_of(
      Found, [&](const auto &Item) { return Item.first == Z + 4; }));
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  llvm::LLVMContext Context;
  const PipelineResult R = Pipeline().run(T.Img, Context, Options);
  ASSERT_TRUE(R.Success) << R.Error;
  const auto It =
      llvm::find_if(R.MedFuncs, [&](const MedFunc &M) { return M.Entry == G; });
  ASSERT_NE(It, R.MedFuncs.end());
  EXPECT_FALSE(hasOp(*It, NdOp::CALL));
  unsigned Stores = 0;
  for (const MedBlock &Block : It->Blocks)
    for (const MedOp &Op : Block.Ops)
      Stores += Op.Opcode == NdOp::STORE;
  EXPECT_GE(Stores, 2u);
  const std::string Text = highCBody(R, T.Img, "main_g");
  ASSERT_FALSE(Text.empty());
  EXPECT_EQ(Text.find("__builtin_trap"), std::string::npos) << Text;
}

TEST(GoAssembly, ADuffsDeviceEntersWithZeroInX15) {
  // Compiled code alone calls a Duff's device routine, at whichever entry,
  // so under the internal ABI it enters with X15 zero: Go 1.18-1.24's
  // `runtime.duffzero` stores X15.
  //   runtime.duffzero: movups [rdi], xmm15; movups [rdi+16], xmm15; ret
  constexpr va_t Zero = kTextVA + 0x100;
  GoTestImage T;
  T.writeText(Zero,
              {0x44, 0x0F, 0x11, 0x3F, 0x44, 0x0F, 0x11, 0x7F, 0x10, 0xC3});
  Symbol Function = Symbol::makeFunc(Zero);
  Function.Name = "runtime.duffzero";
  T.Img.Symbols.push_back(std::move(Function));
  GoModuleInfo Module;
  Module.PclnTabMagic = 0xFFFFFFF1u;
  Module.RegisterArguments = true;
  Module.GoCode = {{Zero, kTextVA + 0x300}};
  Module.GoAssembly = {{Zero, Zero + 0x10}};
  Module.FrameArguments = {{Zero, 0, 0}};
  Module.DuffDevices = {{Zero, Zero + 0x10}};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  llvm::LLVMContext Context;
  const PipelineResult R = Pipeline().run(T.Img, Context, Options);
  ASSERT_TRUE(R.Success) << R.Error;
  const std::string Text = highCBody(R, T.Img, "runtime_duffzero");
  ASSERT_FALSE(Text.empty());
  EXPECT_EQ(Text.find("__builtin_trap"), std::string::npos) << Text;
}

TEST(GoAssembly, ABIInternalAssemblyEntersWithZeroInX15) {
  // Go's internal ABI enters a function with X15 zero, and assembly that
  // takes Go's argument registers follows that ABI:
  // `runtime.memclrNoHeapPointers` clears memory with `movdqu %xmm15, (%rdi)`.
  //   main.clr: movdqu [rax], xmm15; mov [rax+16], rbx; ret
  constexpr va_t Clr = kTextVA + 0x100;
  GoTestImage T;
  T.writeText(Clr,
              {0xF3, 0x44, 0x0F, 0x7F, 0x38, 0x48, 0x89, 0x58, 0x10, 0xC3});
  Symbol Function = Symbol::makeFunc(Clr);
  Function.Name = "main.clr";
  T.Img.Symbols.push_back(std::move(Function));
  GoModuleInfo Module;
  Module.PclnTabMagic = 0xFFFFFFF1u;
  Module.RegisterArguments = true;
  Module.GoCode = {{Clr, kTextVA + 0x300}};
  Module.GoAssembly = {{Clr, Clr + 0x10}};
  Module.FrameArguments = {{Clr, 16, std::nullopt}};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  llvm::LLVMContext Context;
  const PipelineResult R = Pipeline().run(T.Img, Context, Options);
  ASSERT_TRUE(R.Success) << R.Error;
  EXPECT_EQ(parameterRegisters(R, Clr),
            (std::vector<uint64_t>{x86reg::RAX, x86reg::RBX}));
  const std::string Text = highCBody(R, T.Img, "main_clr");
  ASSERT_FALSE(Text.empty());
  EXPECT_EQ(Text.find("__builtin_trap"), std::string::npos) << Text;
}

TEST(GoSyscall, AnOrrMovedNumberBoundsTheArguments) {
  // Go's assembler moves a bitmask constant with ORR: `runtime.osyield` sets
  // its system call number with `orr x8, xzr, #0x7c`, for which Capstone names
  // no MOV alias, MOVZ being able to encode the value.  Lifted as an OR of two
  // operands, the number was no constant the block set, so sched_yield read
  // X0-X5 and every caller passed six unknown arguments.
  //   runtime.osyield: orr x8, xzr, #0x7c; svc #0; ret
  constexpr va_t F = kTextVA + 0x100;
  GoTestImage T(Arch::AArch64);
  T.Img.ELFMetadata.emplace();
  T.Img.ELFMetadata->OSABI = llvm::ELF::ELFOSABI_NONE;
  T.writeText(F, words({0xB27E13E8, 0xD4000001, 0xD65F03C0}));
  Symbol Function = Symbol::makeFunc(F);
  Function.Name = "runtime.osyield";
  T.Img.Symbols.push_back(std::move(Function));
  GoModuleInfo Module;
  Module.PclnTabMagic = 0xFFFFFFF1u;
  Module.RegisterArguments = true;
  Module.GoCode = {{F, kTextVA + 0x300}};
  Module.GoAssembly = {{F, F + 0x10}};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  llvm::LLVMContext Context;
  const PipelineResult R = Pipeline().run(T.Img, Context, Options);
  ASSERT_TRUE(R.Success) << R.Error;
  const auto It =
      llvm::find_if(R.MedFuncs, [&](const MedFunc &M) { return M.Entry == F; });
  ASSERT_NE(It, R.MedFuncs.end());
  EXPECT_TRUE(It->Params.empty()) << It->Params.size();
}

TEST(GoDefer, DeferReturnResumesOnTheFrame) {
  // Go's runtime resumes a frame that deferred at its `deferreturn` call on
  // the frame's own stack pointer, from where the code reloads the named
  // result and returns.  Entered at the stack pointer the function was called
  // with, those loads read its caller's frame, and `main.namedResultRewrite`
  // took a stack argument no caller passed.
  //   main.f: str x30, [sp, #-0x20]!; str x0, [sp, #0x10]; bl main.g
  //           ldr x0, [sp, #0x10]; ldr x30, [sp], #0x20; ret
  //       r:  bl main.g; ldr x0, [sp, #0x10]; ldr x30, [sp], #0x20; ret
  //   main.g: ret
  constexpr va_t F = kTextVA + 0x100;
  constexpr va_t G = kTextVA + 0x180;
  GoTestImage T(Arch::AArch64);
  const auto Call = [&](va_t At) {
    return 0x94000000u | (((G - At) >> 2) & 0x3FFFFFFu);
  };
  T.writeText(
      F, words({0xF81E0FFE, 0xF9000BE0, Call(F + 8), 0xF9400BE0, 0xF84207FE,
                0xD65F03C0, Call(F + 24), 0xF9400BE0, 0xF84207FE, 0xD65F03C0}));
  T.writeText(G, words({0xD65F03C0}));
  for (const auto &[Entry, Name] :
       {std::pair{F, "main.f"}, std::pair{G, "main.g"}}) {
    Symbol Function = Symbol::makeFunc(Entry);
    Function.Name = Name;
    T.Img.Symbols.push_back(std::move(Function));
  }
  GoModuleInfo Module;
  Module.PclnTabMagic = 0xFFFFFFF1u;
  Module.RegisterArguments = true;
  Module.GoCode = {{F, kTextVA + 0x300}};
  Module.FrameArguments = {{F, 8, 1}, {G, 0, 0}};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  ExceptionFunction EH;
  EH.CodeRange = {F, F + 40};
  EH.Kind = RuntimeFunctionKind::Primary;
  EH.Encoding = ExceptionEncoding::GoFuncTable;
  GoFunctionEH Go;
  Go.EntryVA = F;
  Go.DeferReturnOffset = 24;
  Go.FrameSize = 0x20;
  Go.UsesOpenCodedDefers = true;
  EH.Go = std::move(Go);
  T.Img.ExceptionMetadata.Functions.push_back(std::move(EH));
  T.Img.ExceptionMetadata.rebuildIndex();
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  llvm::LLVMContext Context;
  const PipelineResult R = Pipeline().run(T.Img, Context, Options);
  ASSERT_TRUE(R.Success) << R.Error;
  const auto It =
      llvm::find_if(R.MedFuncs, [&](const MedFunc &M) { return M.Entry == F; });
  ASSERT_NE(It, R.MedFuncs.end());
  EXPECT_EQ(It->Params.size(), 1u);
}

TEST(GoInternalABI, AClosureReadsTheContextItsCallerPasses) {
  // A call through a Go func value passes the value's address in RDX (arm64
  // R26), through which a closure reads what it captured.  No model defined
  // the incoming register, and each capture read printed an unknown value.
  //   main.f.func1: mov rax, [rdx+8]; add rax, [rdx+16]; ret
  //   (arm64)       ldr x0, [x26, #8]; ldr x1, [x26, #16]; add x0, x0, x1
  //                 ret
  for (const Arch A : {Arch::X64, Arch::AArch64}) {
    SCOPED_TRACE(static_cast<int>(A));
    constexpr va_t F = kTextVA + 0x100;
    GoTestImage T(A);
    if (A == Arch::X64)
      T.writeText(F, {0x48, 0x8B, 0x42, 0x08, 0x48, 0x03, 0x42, 0x10, 0xC3});
    else
      T.writeText(F, words({0xF9400740, 0xF9400B41, 0x8B010000, 0xD65F03C0}));
    Symbol Function = Symbol::makeFunc(F);
    Function.Name = "main.f.func1";
    T.Img.Symbols.push_back(std::move(Function));
    GoModuleInfo Module;
    Module.PclnTabMagic = 0xFFFFFFF1u;
    Module.RegisterArguments = true;
    Module.GoCode = {{kTextVA, kTextVA + 0x400}};
    T.Img.ExceptionMetadata.GoModule = std::move(Module);
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    llvm::LLVMContext Context;
    const PipelineResult R = Pipeline().run(T.Img, Context, Options);
    ASSERT_TRUE(R.Success) << R.Error;
    EXPECT_EQ(R.MedIRVerifierFailures, 0u);
    const std::string Text = highCBody(R, T.Img, "main_f_func1");
    ASSERT_FALSE(Text.empty());
    EXPECT_NE(Text.find(A == Arch::X64 ? "movq %%rdx" : "mov %0, x26"),
              std::string::npos)
        << Text;
    EXPECT_EQ(Text.find("__builtin_trap"), std::string::npos) << Text;
  }
}

TEST(GoInternalABI, ARegisterReadAfterACallIsNoContext) {
  // The context is the register's incoming value.  Where a function reads
  // the register only after a call that writes it, the first version of it is
  // the call's clobber, and taking that version for the incoming value
  // defined it twice: Go 1.26's `internal/cpu.doinit` and `runtime.mapaccess2`
  // were refused.
  //   main.f: lea rdx, [rax+1]; ret
  //   main.g: call main.f; mov rax, rdx; ret
  //   (arm64) main.f: add x26, x0, #1; ret
  //           main.g: stp x29, x30, [sp, #-16]!; mov x29, sp; bl main.f
  //                   mov x0, x26; ldp x29, x30, [sp], #16; ret
  for (const Arch A : {Arch::X64, Arch::AArch64}) {
    SCOPED_TRACE(static_cast<int>(A));
    constexpr va_t F = kTextVA + 0x100;
    constexpr va_t G = kTextVA + 0x180;
    GoTestImage T(A);
    if (A == Arch::X64) {
      T.writeText(F, {0x48, 0x8D, 0x50, 0x01, 0xC3});
      const auto Rel = static_cast<uint32_t>(F - (G + 5));
      T.writeText(G, {0xE8, static_cast<uint8_t>(Rel),
                      static_cast<uint8_t>(Rel >> 8),
                      static_cast<uint8_t>(Rel >> 16),
                      static_cast<uint8_t>(Rel >> 24), 0x48, 0x89, 0xD0, 0xC3});
    } else {
      T.writeText(F, words({0x9100041A, 0xD65F03C0}));
      const uint32_t Call = 0x94000000u | (((F - (G + 8)) >> 2) & 0x3FFFFFFu);
      T.writeText(G, words({0xA9BF7BFD, 0x910003FD, Call, 0xAA1A03E0,
                            0xA8C17BFD, 0xD65F03C0}));
    }
    for (const auto &[Entry, Name] :
         {std::pair{F, "main.f"}, std::pair{G, "main.g"}}) {
      Symbol Function = Symbol::makeFunc(Entry);
      Function.Name = Name;
      T.Img.Symbols.push_back(std::move(Function));
    }
    GoModuleInfo Module;
    Module.PclnTabMagic = 0xFFFFFFF1u;
    Module.RegisterArguments = true;
    Module.GoCode = {{kTextVA, kTextVA + 0x400}};
    T.Img.ExceptionMetadata.GoModule = std::move(Module);
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    llvm::LLVMContext Context;
    const PipelineResult R = Pipeline().run(T.Img, Context, Options);
    ASSERT_TRUE(R.Success) << R.Error;
    EXPECT_EQ(R.MedIRVerifierFailures, 0u);
    const std::string Text = highCBody(R, T.Img, "main_g");
    ASSERT_FALSE(Text.empty());
    EXPECT_EQ(Text.find(A == Arch::X64 ? "movq %%rdx" : "mov %0, x26"),
              std::string::npos)
        << Text;
  }
}

TEST(GoStackABI, AClosureReadsTheContextItsCallerPasses) {
  // Before Go's internal ABI a call through a func value passes the closure
  // in RDX (arm64 R26) too, and a closure, which takes no register argument,
  // reads what it captured through it.  HighC printed its loads as reads of
  // its own frame (`var_30`), LLVM C as reads through an unknown register.
  //   main.f.func1: sub rsp, 0x18; mov [rsp+0x10], rbp; lea rbp, [rsp+0x10]
  //                 mov rax, [rdx+0x30]; add rax, [rdx+0x38]
  //                 mov [rsp+0x20], rax; mov rbp, [rsp+0x10]; add rsp, 0x18
  //                 ret
  //   (arm64)       sub sp, sp, #32; ldr x0, [x26, #0x30]
  //                 ldr x1, [x26, #0x38]; add x0, x0, x1; str x0, [sp, #40]
  //                 add sp, sp, #32; ret
  for (const Arch A : {Arch::X64, Arch::AArch64}) {
    SCOPED_TRACE(static_cast<int>(A));
    constexpr va_t F = kTextVA + 0x100;
    GoTestImage T(A);
    if (A == Arch::X64)
      T.writeText(F,
                  {0x48, 0x83, 0xEC, 0x18, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48,
                   0x8D, 0x6C, 0x24, 0x10, 0x48, 0x8B, 0x42, 0x30, 0x48, 0x03,
                   0x42, 0x38, 0x48, 0x89, 0x44, 0x24, 0x20, 0x48, 0x8B, 0x6C,
                   0x24, 0x10, 0x48, 0x83, 0xC4, 0x18, 0xC3});
    else
      T.writeText(F, words({0xD10083FF, 0xF9401B40, 0xF9401F41, 0x8B010000,
                            0xF90017E0, 0x910083FF, 0xD65F03C0}));
    Symbol Function = Symbol::makeFunc(F);
    Function.Name = "main.f.func1";
    T.Img.Symbols.push_back(std::move(Function));
    GoModuleInfo Module;
    Module.StackArguments = true;
    Module.GoCode = {{kTextVA, kTextVA + 0x400}};
    Module.FrameArguments = {{F, 8}};
    T.Img.ExceptionMetadata.GoModule = std::move(Module);
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    llvm::LLVMContext Context;
    const PipelineResult R = Pipeline().run(T.Img, Context, Options);
    ASSERT_TRUE(R.Success) << R.Error;
    EXPECT_EQ(R.MedIRVerifierFailures, 0u);
    const std::string Text = highCBody(R, T.Img, "main_f_func1");
    ASSERT_FALSE(Text.empty());
    EXPECT_NE(Text.find(A == Arch::X64 ? "movq %%rdx" : "mov %0, x26"),
              std::string::npos)
        << Text;
    EXPECT_EQ(Text.find("var_30"), std::string::npos) << Text;
    EXPECT_EQ(Text.find("var_38"), std::string::npos) << Text;
    EXPECT_EQ(Text.find("__builtin_trap"), std::string::npos) << Text;
  }
}

TEST(GoStackABI, AnARM64GoroutineRegisterHoldsTheCurrentGoroutine) {
  // Go has kept the arm64 goroutine in R28 since its port, before the
  // internal ABI too.  Only register-ABI code read the register as the
  // goroutine, so a Go 1.15 function's loads through it read an unknown
  // register.
  //   main.f: ldr x0, [x28, #48]; str x0, [sp, #8]; ret
  constexpr va_t F = kTextVA + 0x100;
  GoTestImage T(Arch::AArch64);
  T.writeText(F, words({0xF9401B80, 0xF90007E0, 0xD65F03C0}));
  Symbol Function = Symbol::makeFunc(F);
  Function.Name = "main.f";
  T.Img.Symbols.push_back(std::move(Function));
  GoModuleInfo Module;
  Module.StackArguments = true;
  Module.GoCode = {{kTextVA, kTextVA + 0x400}};
  Module.FrameArguments = {{F, 8}};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  llvm::LLVMContext Context;
  const PipelineResult R = Pipeline().run(T.Img, Context, Options);
  ASSERT_TRUE(R.Success) << R.Error;
  EXPECT_EQ(R.MedIRVerifierFailures, 0u);
  const std::string Text = highCBody(R, T.Img, "main_f");
  ASSERT_FALSE(Text.empty());
  EXPECT_NE(Text.find("mov %0, x28"), std::string::npos) << Text;
  EXPECT_EQ(Text.find("__builtin_trap"), std::string::npos) << Text;
}

TEST(GoInternalABI, ABranchDecidesTheSelectionBelowIt) {
  // main.f(p, n uintptr) uintptr counts n's trailing zeros where n is not
  // zero and p is 1, as `runtime.(*pageCache).alloc` does: `bsf` keeps RCX
  // where RBX is zero, which the test two blocks above has ruled out.
  //   main.f: cmp rsp, [r14+0x10]; jbe grow; test rbx, rbx; je zero
  //           cmp rax, 1; jne other; bsf rcx, rbx; mov rax, rcx; ret
  //   other:  lea rax, [rax+rbx]; ret
  //   zero:   xor eax, eax; ret
  //   grow:   spill RAX and RBX; call morestack; reload them; jmp main.f
  // The selection of RCX's old value printed as an unknown value.
  constexpr va_t F = kTextVA + 0x100;
  constexpr va_t Grow = F + 0x40;
  GoTestImage T;
  T.writeText(F,
              {0x49, 0x3B, 0x66, 0x10, 0x76, 0x1B, 0x48, 0x85, 0xDB, 0x74, 0x13,
               0x48, 0x83, 0xF8, 0x01, 0x75, 0x08, 0x48, 0x0F, 0xBC, 0xCB, 0x48,
               0x89, 0xC8, 0xC3, 0x48, 0x8D, 0x04, 0x18, 0xC3, 0x31, 0xC0, 0xC3,
               0x48, 0x89, 0x44, 0x24, 0x08, 0x48, 0x89, 0x5C, 0x24, 0x10, 0xE8,
               0x10, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x44, 0x24, 0x08, 0x48, 0x8B,
               0x5C, 0x24, 0x10, 0xEB, 0xC4, 0x0F, 0x1F, 0x40, 0x00, 0xC3});
  for (const auto &[Entry, Name] :
       {std::pair{F, "main.f"}, std::pair{Grow, "runtime.morestack_noctxt"}}) {
    Symbol Function = Symbol::makeFunc(Entry);
    Function.Name = Name;
    T.Img.Symbols.push_back(std::move(Function));
  }
  GoModuleInfo Module;
  Module.PclnTabMagic = 0xFFFFFFF1u;
  Module.RegisterArguments = true;
  Module.GoCode = {{kTextVA, kTextVA + 0x400}};
  Module.MorestackEntries = {Grow};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  llvm::LLVMContext Context;
  const PipelineResult R = Pipeline().run(T.Img, Context, Options);
  ASSERT_TRUE(R.Success) << R.Error;
  EXPECT_EQ(R.MedIRVerifierFailures, 0u);
  const auto Function =
      llvm::find_if(R.MedFuncs, [&](const MedFunc &M) { return M.Entry == F; });
  ASSERT_NE(Function, R.MedFuncs.end());
  EXPECT_EQ(Function->Params.size(), 2u);
  EXPECT_TRUE(llvm::none_of(Function->Blocks, [](const MedBlock &Block) {
    return llvm::any_of(
        Block.Ops, [](const MedOp &Op) { return Op.Opcode == NdOp::SELECT; });
  }));
  const std::string Text = highCBody(R, T.Img, "main_f");
  ASSERT_FALSE(Text.empty());
  EXPECT_EQ(Text.find("__builtin_trap"), std::string::npos) << Text;
}

TEST(GoInternalABI, TheFramePointerAFunctionEntersWithIsItsCallersFrame) {
  // runtime.getfp: mov rax, rbp; ret
  // main.g:        push rbp; mov rbp, rsp; call runtime.getfp
  //                mov rax, [rax]; pop rbp; ret
  // getfp returns its caller's frame, from which `runtime.traceStack` and
  // `getcallerfp` walk the frames.  Taken for a register no argument set,
  // it made getfp return nothing, and each read of its result unknown.
  constexpr va_t GetFp = kTextVA + 0x100;
  constexpr va_t G = GetFp + 0x10;
  GoTestImage T;
  T.writeText(GetFp, {0x48, 0x89, 0xE8, 0xC3});
  std::vector<uint8_t> Caller = {0x55, 0x48, 0x89, 0xE5};
  const std::vector<uint8_t> Call = makeCall(G + Caller.size(), GetFp);
  Caller.insert(Caller.end(), Call.begin(), Call.end());
  Caller.insert(Caller.end(), {0x48, 0x8B, 0x00, 0x5D, 0xC3});
  T.writeText(G, Caller);
  for (const auto &[Entry, Name] :
       {std::pair{GetFp, "runtime.getfp"}, std::pair{G, "main.g"}}) {
    Symbol Function = Symbol::makeFunc(Entry);
    Function.Name = Name;
    T.Img.Symbols.push_back(std::move(Function));
  }
  GoModuleInfo Module;
  Module.PclnTabMagic = 0xFFFFFFF1u;
  Module.RegisterArguments = true;
  Module.GoCode = {{kTextVA, kTextVA + 0x400}};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  llvm::LLVMContext Context;
  const PipelineResult R = Pipeline().run(T.Img, Context, Options);
  ASSERT_TRUE(R.Success) << R.Error;
  EXPECT_EQ(R.MedIRVerifierFailures, 0u);
  const auto Function = llvm::find_if(
      R.MedFuncs, [&](const MedFunc &M) { return M.Entry == GetFp; });
  ASSERT_NE(Function, R.MedFuncs.end());
  ASSERT_TRUE(Function->ReturnType);
  EXPECT_NE(Function->ReturnType->Kind, NdTypeKind::Void);
  const std::string Text = highCBody(R, T.Img, "runtime_getfp");
  ASSERT_FALSE(Text.empty());
  EXPECT_NE(Text.find("__builtin_frame_address(1)"), std::string::npos) << Text;
  const std::string CallerText = highCBody(R, T.Img, "main_g");
  ASSERT_FALSE(CallerText.empty());
  EXPECT_EQ(CallerText.find("__builtin_trap"), std::string::npos) << CallerText;
}

TEST(GoStackGrowth, AFunctionOfFloatsEntersItsBodyDirectly) {
  // main.m(x float64) (float64, float64) returns (x, x) for zero, (0, x)
  // otherwise, and for a negative x the results of m(-x) the other way
  // round; main.user adds the two.  `runtime.modf` has the shape.
  //   main.m: cmp rsp, [r14+0x10]; jbe grow; push rbp; mov rbp, rsp
  //           sub rsp, 8; xorps xmm2, xmm2; ucomisd xmm2, xmm0; ja neg
  //           ucomisd xmm0, xmm2; jne zero; jp zero
  //           movups xmm1, xmm0; add rsp, 8; pop rbp; ret
  //     zero: movups xmm1, xmm0; movups xmm0, xmm2; add rsp, 8; pop rbp; ret
  //     neg:  subsd xmm2, xmm0; movups xmm0, xmm2; call main.m
  //           movups xmm2, xmm0; movups xmm0, xmm1; movups xmm1, xmm2
  //           add rsp, 8; pop rbp; ret
  //     grow: movsd [rsp+8], xmm0; call morestack; movsd xmm0, [rsp+8]
  //           jmp main.m
  //   main.user: the same entry and growth path round
  //           call main.m; addsd xmm0, xmm1
  // Each takes one double and keeps no growth path.  `main.m` returns RAX
  // beside X0 and X1 only where its own call left it: the return two paths
  // reach with RAX as the function was entered with hands back an unknown
  // value there, where the function was refused once its body was no loop.
  constexpr va_t M = kTextVA + 0x100;
  constexpr va_t User = M + 0x70;
  constexpr va_t Grow = M + 0xA0;
  GoTestImage T;
  T.writeText(M,
              {0x49, 0x3B, 0x66, 0x10, 0x76, 0x49, 0x55, 0x48, 0x89, 0xE5, 0x48,
               0x83, 0xEC, 0x08, 0x0F, 0x57, 0xD2, 0x66, 0x0F, 0x2E, 0xD0, 0x77,
               0x1D, 0x66, 0x0F, 0x2E, 0xC2, 0x75, 0x0B, 0x7A, 0x09, 0x0F, 0x10,
               0xC8, 0x48, 0x83, 0xC4, 0x08, 0x5D, 0xC3, 0x0F, 0x10, 0xC8, 0x0F,
               0x10, 0xC2, 0x48, 0x83, 0xC4, 0x08, 0x5D, 0xC3, 0xF2, 0x0F, 0x5C,
               0xD0, 0x0F, 0x10, 0xC2, 0xE8, 0xC0, 0xFF, 0xFF, 0xFF, 0x0F, 0x10,
               0xD0, 0x0F, 0x10, 0xC1, 0x0F, 0x10, 0xCA, 0x48, 0x83, 0xC4, 0x08,
               0x5D, 0xC3, 0xF2, 0x0F, 0x11, 0x44, 0x24, 0x08, 0xE8, 0x46, 0x00,
               0x00, 0x00, 0xF2, 0x0F, 0x10, 0x44, 0x24, 0x08, 0xEB, 0x9E, 0x0F,
               0x1F, 0x00, 0x66, 0x66, 0x2E, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x49, 0x3B, 0x66, 0x10, 0x76, 0x17, 0x55, 0x48, 0x89,
               0xE5, 0x48, 0x83, 0xEC, 0x08, 0xE8, 0x7D, 0xFF, 0xFF, 0xFF, 0xF2,
               0x0F, 0x58, 0xC1, 0x48, 0x83, 0xC4, 0x08, 0x5D, 0xC3, 0xF2, 0x0F,
               0x11, 0x44, 0x24, 0x08, 0xE8, 0x08, 0x00, 0x00, 0x00, 0xF2, 0x0F,
               0x10, 0x44, 0x24, 0x08, 0xEB, 0xD0, 0xC3});
  for (const auto &[Entry, Name] :
       {std::pair{M, "main.m"}, std::pair{User, "main.user"},
        std::pair{Grow, "runtime.morestack_noctxt"}}) {
    Symbol Function = Symbol::makeFunc(Entry);
    Function.Name = Name;
    T.Img.Symbols.push_back(std::move(Function));
  }
  GoModuleInfo Module;
  Module.PclnTabMagic = 0xFFFFFFF1u;
  Module.RegisterArguments = true;
  Module.GoCode = {{kTextVA, kTextVA + 0x400}};
  Module.MorestackEntries = {Grow};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  llvm::LLVMContext Context;
  const PipelineResult R = Pipeline().run(T.Img, Context, Options);
  ASSERT_TRUE(R.Success) << R.Error;
  EXPECT_EQ(R.MedIRVerifierFailures, 0u);
  for (const va_t Entry : {M, User}) {
    SCOPED_TRACE(Entry);
    const auto It = llvm::find_if(
        R.MedFuncs, [&](const MedFunc &F) { return F.Entry == Entry; });
    ASSERT_NE(It, R.MedFuncs.end());
    ASSERT_EQ(It->Params.size(), 1u);
    EXPECT_EQ(It->Params[0].RegOff, x86reg::vectorReg(0));
    const auto Scalar = It->FPParamScalarBytes.find(x86reg::vectorReg(0));
    ASSERT_NE(Scalar, It->FPParamScalarBytes.end());
    EXPECT_EQ(Scalar->second, 8u);
    EXPECT_TRUE(llvm::none_of(It->Blocks, [&](const MedBlock &Block) {
      return llvm::any_of(Block.Ops, [&](const MedOp &Op) {
        return Op.Opcode == NdOp::CALL && Op.NumInputs &&
               Op.Inputs[0].isConst() && Op.Inputs[0].ConstVal == Grow;
      });
    }));
  }
  const std::string Text = highCBody(R, T.Img, "main_m");
  ASSERT_FALSE(Text.empty()) << R.Error;
  EXPECT_NE(Text.find("double arg0"), std::string::npos) << Text;
  EXPECT_EQ(Text.find("morestack"), std::string::npos) << Text;
  EXPECT_EQ(Text.find("while"), std::string::npos) << Text;
  const std::string Caller = highCBody(R, T.Img, "main_user");
  ASSERT_FALSE(Caller.empty()) << R.Error;
  EXPECT_NE(Caller.find("double arg0"), std::string::npos) << Caller;
  EXPECT_EQ(Caller.find("morestack"), std::string::npos) << Caller;
}

TEST(GoInternalABI, ACallWithAFloatPassesTheIntegersItsSummaryBound) {
  // main.f(p, n, c, x float64) stores n, c and x through p and returns
  // p + 24 and c + 1; main.g(x) passes it a buffer of its own, 0, 5 and x,
  // and stores the second result through the first.
  //   main.f: mov [rax], rbx; mov [rax+8], rcx; movsd [rax+16], xmm0
  //           add rax, 24; lea rbx, [rcx+1]; ret
  //   main.g: sub rsp, 0x28; lea rax, [rsp]; xor ebx, ebx; mov ecx, 5
  //           call main.f; mov [rax], rbx; add rsp, 0x28; ret
  // A summary that lists a floating argument was no integer summary to call
  // recovery, which scanned for the registers' setup instead of taking the
  // operands the summary bound.  Register DCE keeps that setup for the
  // platform's argument registers alone, and the call's own results end
  // what RAX and RBX held, which are none of System V's: the buffer and the
  // 0 printed unknown, as `strconv.genericFtoa(?, ?, 24, 'g', -1, 64, x)`.
  constexpr va_t F = kTextVA + 0x100;
  constexpr va_t G = F + 0x20;
  GoTestImage T =
      goFunctions({{{F, "main.f"}, {0x48, 0x89, 0x18, 0x48, 0x89, 0x48, 0x08,
                                    0xF2, 0x0F, 0x11, 0x40, 0x10, 0x48, 0x83,
                                    0xC0, 0x18, 0x48, 0x8D, 0x59, 0x01, 0xC3}},
                   {{G, "main.g"},
                    {0x48, 0x83, 0xEC, 0x28, 0x48, 0x8D, 0x04, 0x24, 0x31, 0xDB,
                     0xB9, 0x05, 0x00, 0x00, 0x00, 0xE8, 0xCC, 0xFF, 0xFF, 0xFF,
                     0x48, 0x89, 0x18, 0x48, 0x83, 0xC4, 0x28, 0xC3}}});
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  llvm::LLVMContext Context;
  const PipelineResult R = Pipeline().run(T.Img, Context, Options);
  ASSERT_TRUE(R.Success) << R.Error;
  EXPECT_EQ(R.MedIRVerifierFailures, 0u);
  const auto Caller =
      llvm::find_if(R.MedFuncs, [&](const MedFunc &M) { return M.Entry == G; });
  ASSERT_NE(Caller, R.MedFuncs.end());
  const auto Info =
      llvm::find_if(Caller->CallInfos, [&](const MedCallInfo &Call) {
        return Call.TargetAddr == F;
      });
  ASSERT_NE(Info, Caller->CallInfos.end());
  ASSERT_GE(Info->Args.size(), 3u);
  ASSERT_TRUE(Info->Args[1].isConst());
  EXPECT_EQ(Info->Args[1].ConstVal, 0u);
  ASSERT_TRUE(Info->Args[2].isConst());
  EXPECT_EQ(Info->Args[2].ConstVal, 5u);
  const std::string Text = highCBody(R, T.Img, "main_g");
  ASSERT_FALSE(Text.empty());
  EXPECT_EQ(Text.find("__builtin_trap"), std::string::npos) << Text;
  EXPECT_NE(Text.find("&stack_storage"), std::string::npos) << Text;
  EXPECT_NE(Text.find(", 0, 5, arg0)"), std::string::npos) << Text;
}

TEST(GoInternalABI, AFuncValueCallReturnsItsFloatResults) {
  // Go's internal ABI returns floating results in X0-X14 (arm64 F0-F15),
  // numbered apart from its integer ones.  A call through a func value
  // returned only the integer ones, so a read of X0 after it read what the
  // call clobbered.
  //   main.g: push rbp; mov rbp, rsp; sub rsp, 16; mov [rsp], rbx
  //           mov rdx, rax; call [rdx]; mov rcx, [rsp]; movsd [rcx], xmm0
  //           add rsp, 16; pop rbp; ret
  //   (arm64) stp x29, x30, [sp, #-32]!; mov x29, sp; str x1, [sp, #16]
  //           mov x26, x0; ldr x2, [x26]; blr x2; ldr x1, [sp, #16]
  //           str d0, [x1]; ldp x29, x30, [sp], #32; ret
  for (const Arch A : {Arch::X64, Arch::AArch64}) {
    SCOPED_TRACE(static_cast<int>(A));
    constexpr va_t G = kTextVA + 0x100;
    GoTestImage T(A);
    if (A == Arch::X64)
      T.writeText(G, {0x55, 0x48, 0x89, 0xE5, 0x48, 0x83, 0xEC, 0x10,
                      0x48, 0x89, 0x1C, 0x24, 0x48, 0x89, 0xC2, 0xFF,
                      0x12, 0x48, 0x8B, 0x0C, 0x24, 0xF2, 0x0F, 0x11,
                      0x01, 0x48, 0x83, 0xC4, 0x10, 0x5D, 0xC3});
    else
      T.writeText(G, words({0xA9BE7BFD, 0x910003FD, 0xF9000BE1, 0xAA0003FA,
                            0xF9400342, 0xD63F0040, 0xF9400BE1, 0xFD000020,
                            0xA8C27BFD, 0xD65F03C0}));
    Symbol Function = Symbol::makeFunc(G);
    Function.Name = "main.g";
    T.Img.Symbols.push_back(std::move(Function));
    GoModuleInfo Module;
    Module.PclnTabMagic = 0xFFFFFFF1u;
    Module.RegisterArguments = true;
    Module.GoCode = {{kTextVA, kTextVA + 0x400}};
    T.Img.ExceptionMetadata.GoModule = std::move(Module);
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    llvm::LLVMContext Context;
    const PipelineResult R = Pipeline().run(T.Img, Context, Options);
    ASSERT_TRUE(R.Success) << R.Error;
    EXPECT_EQ(R.MedIRVerifierFailures, 0u);
    const std::string Text = highCBody(R, T.Img, "main_g");
    ASSERT_FALSE(Text.empty());
    EXPECT_EQ(Text.find("__builtin_trap"), std::string::npos) << Text;
    EXPECT_NE(Text.find("double"), std::string::npos) << Text;
  }
}

TEST(GoAssembly, AReflectCallHelperStoresWhatItsFuncValueReturns) {
  // Reflection's call helpers (`runtime.callNNN`) load a call's argument
  // registers from an internal/abi.RegArgs with `runtime.unspillArgs`, call
  // a func value, and store every result register back with
  // `runtime.spillArgs`.  Each transfer runs in its caller's place, and the
  // func value enters Go code by the internal ABI, so the stores read the
  // call's results: each of them read an unknown value.
  //   runtime.unspillArgs: mov rax, [r12]; mov rbx, [r12+8]
  //                        movsd xmm0, [r12+72]; ret
  //   runtime.spillArgs:   mov [r12], rax; mov [r12+8], rbx
  //                        movsd [r12+72], xmm0; ret
  //   main.call16: push rbp; mov rbp, rsp; sub rsp, 16; mov [rsp], rsi
  //                mov r12, rsi; call runtime.unspillArgs; mov rdx, rdi
  //                mov r12, [rdx]; call r12; mov r12, [rsp]
  //                call runtime.spillArgs; add rsp, 16; pop rbp; ret
  constexpr va_t Helper = kTextVA + 0x100;
  constexpr va_t Unspill = kTextVA + 0x200;
  constexpr va_t Spill = kTextVA + 0x240;
  GoTestImage T(Arch::X64);
  T.writeText(Unspill, {0x49, 0x8B, 0x04, 0x24, 0x49, 0x8B, 0x5C, 0x24, 0x08,
                        0xF2, 0x41, 0x0F, 0x10, 0x44, 0x24, 0x48, 0xC3});
  T.writeText(Spill, {0x49, 0x89, 0x04, 0x24, 0x49, 0x89, 0x5C, 0x24, 0x08,
                      0xF2, 0x41, 0x0F, 0x11, 0x44, 0x24, 0x48, 0xC3});
  const auto Rel = [](va_t Target, va_t Next) {
    const auto Value = static_cast<uint32_t>(Target - Next);
    return std::array<uint8_t, 4>{
        static_cast<uint8_t>(Value), static_cast<uint8_t>(Value >> 8),
        static_cast<uint8_t>(Value >> 16), static_cast<uint8_t>(Value >> 24)};
  };
  const auto ToUnspill = Rel(Unspill, Helper + 20);
  const auto ToSpill = Rel(Spill, Helper + 38);
  T.writeText(
      Helper,
      {0x55,         0x48,         0x89, 0xE5, 0x48,         0x83,
       0xEC,         0x10,         0x48, 0x89, 0x34,         0x24,
       0x49,         0x89,         0xF4, 0xE8, ToUnspill[0], ToUnspill[1],
       ToUnspill[2], ToUnspill[3], 0x48, 0x89, 0xFA,         0x4C,
       0x8B,         0x22,         0x41, 0xFF, 0xD4,         0x4C,
       0x8B,         0x24,         0x24, 0xE8, ToSpill[0],   ToSpill[1],
       ToSpill[2],   ToSpill[3],   0x48, 0x83, 0xC4,         0x10,
       0x5D,         0xC3});
  for (const auto &[Entry, Name] : {std::pair{Helper, "main.call16"},
                                    std::pair{Unspill, "runtime.unspillArgs"},
                                    std::pair{Spill, "runtime.spillArgs"}}) {
    Symbol Function = Symbol::makeFunc(Entry);
    Function.Name = Name;
    T.Img.Symbols.push_back(std::move(Function));
  }
  GoModuleInfo Module;
  Module.PclnTabMagic = 0xFFFFFFF1u;
  Module.RegisterArguments = true;
  Module.GoCode = {{kTextVA, kTextVA + 0x400}};
  Module.GoAssembly = {{Helper, Helper + 0x40},
                       {Unspill, Unspill + 0x40},
                       {Spill, Spill + 0x40}};
  Module.RegisterArgumentTransfers = {{Unspill, Unspill + 0x40},
                                      {Spill, Spill + 0x40}};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  llvm::LLVMContext Context;
  const PipelineResult R = Pipeline().run(T.Img, Context, Options);
  ASSERT_TRUE(R.Success) << R.Error;
  EXPECT_EQ(R.MedIRVerifierFailures, 0u);
  const std::string Text = highCBody(R, T.Img, "main_call16");
  ASSERT_FALSE(Text.empty());
  EXPECT_EQ(Text.find("__builtin_trap"), std::string::npos) << Text;
  EXPECT_EQ(Text.find("spillArgs"), std::string::npos) << Text;
  EXPECT_NE(Text.find("double"), std::string::npos) << Text;
  // The func value takes the two words and the double the transfer loads,
  // in Go's registers, and nothing the helper was entered with: RDI and RSI,
  // its own parameters, are Go's fourth and fifth by chance.
  std::smatch Call;
  ASSERT_TRUE(std::regex_search(
      Text, Call,
      std::regex(
          R"(\(\*\(void \*\*\)\(arg0\)\)\)\((\w+), (\w+), )"
          R"(\(__int128\)\(uint64_t\)\(\*\(uint64_t \*\)\(arg1 \+ 0x48\)\)\);)")))
      << Text;
  EXPECT_NE(Text.find(Call[1].str() + " = *(int64_t *)arg1;"),
            std::string::npos)
      << Text;
  EXPECT_NE(Text.find(Call[2].str() + " = *(int64_t *)(arg1 + 8);"),
            std::string::npos)
      << Text;
  EXPECT_NE(Text.find("main_call16(void* arg0, void* arg1)"), std::string::npos)
      << Text;
}

TEST(GoWriteBarriers, AStoringBarrierReadsItsGoroutine) {
  // Go 1.17 to 1.20's storing barrier, which only compiled code calls, finds
  // its buffer through the goroutine register and saves every register it
  // keeps round the flush, the two the convention gives no value among them:
  //   main.flush: ret
  //   runtime.gcWriteBarrier: sub rsp, 0x28; mov r13, [r14+0x30]
  //           mov [r13+8], rax; mov [rdi], rax; lea rcx, [rsp+0x20]
  //           mov [rsp], rcx; mov [rsp+8], r10; mov [rsp+0x10], r11
  //           call main.flush; mov r10, [rsp+8]; mov r11, [rsp+0x10]
  //           add rsp, 0x28; ret
  constexpr va_t Flush = kTextVA + 0x180;
  constexpr va_t BarrierEntry = kTextVA + 0x200;
  GoTestImage T(Arch::X64);
  T.writeText(Flush, {0xC3});
  std::vector<uint8_t> Code = {
      0x48, 0x83, 0xEC, 0x28, 0x4D, 0x8B, 0x6E, 0x30, 0x49, 0x89, 0x45, 0x08,
      0x48, 0x89, 0x07, 0x48, 0x8D, 0x4C, 0x24, 0x20, 0x48, 0x89, 0x0C, 0x24,
      0x4C, 0x89, 0x54, 0x24, 0x08, 0x4C, 0x89, 0x5C, 0x24, 0x10};
  const std::vector<uint8_t> Call = makeCall(BarrierEntry + Code.size(), Flush);
  Code.insert(Code.end(), Call.begin(), Call.end());
  Code.insert(Code.end(), {0x4C, 0x8B, 0x54, 0x24, 0x08, 0x4C, 0x8B, 0x5C, 0x24,
                           0x10, 0x48, 0x83, 0xC4, 0x28, 0xC3});
  T.writeText(BarrierEntry, Code);
  for (const auto &[Address, Name] :
       {std::pair{Flush, "main.flush"},
        std::pair{BarrierEntry, "runtime.gcWriteBarrier"}}) {
    Symbol Function = Symbol::makeFunc(Address);
    Function.Name = Name;
    T.Img.Symbols.push_back(std::move(Function));
  }
  GoModuleInfo Module;
  Module.PclnTabMagic = 0xFFFFFFF1u;
  Module.RegisterArguments = true;
  Module.GoCode = {{kTextVA + 0x100, kTextVA + 0x400}};
  Module.GoAssembly = {{BarrierEntry, BarrierEntry + 0x40}};
  Module.FrameArguments = {{Flush, 0, 0}, {BarrierEntry}};
  GoWriteBarrier Barrier;
  Barrier.Entry = BarrierEntry;
  Barrier.Kind = GoWriteBarrierKind::Store;
  Barrier.Destination = x86reg::RDI;
  Barrier.Value = x86reg::RAX;
  Module.WriteBarriers = {Barrier};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  llvm::LLVMContext Context;
  const PipelineResult R = Pipeline().run(T.Img, Context, Options);
  ASSERT_TRUE(R.Success) << R.Error;
  EXPECT_EQ(R.MedIRVerifierFailures, 0u);
  const std::string Text = highCBody(R, T.Img, "runtime_gcWriteBarrier");
  // It reads the goroutine where its caller keeps it, not a register with no
  // value at entry ...
  EXPECT_NE(Text.find("%%r14"), std::string::npos) << Text;
  EXPECT_NE(Text.find("(g + 0x30)"), std::string::npos) << Text;
  // ... and that read, the first statement, ends no prefix of frame
  // addresses: the saves of R10 and R11, which nothing reads back, still go,
  // while the frame word the function fills stays.
  EXPECT_EQ(Text.find("__builtin_trap"), std::string::npos) << Text;
  EXPECT_EQ(Text.find("var_m20"), std::string::npos) << Text;
  EXPECT_EQ(Text.find("var_m18"), std::string::npos) << Text;
  EXPECT_NE(Text.find("stack_storage.var_m28 = "), std::string::npos) << Text;
  EXPECT_NE(Text.find("main_flush();"), std::string::npos) << Text;
}

namespace {
/// The C of Go 1.21's shared write barrier body and of one numbered barrier
/// that jumps to it, and the body's parameters.
///   main.flush: ret
///   gcWriteBarrier:
///           sub rsp, 0x18; mov [rsp+8], r12
///        1: mov r12, [r14+0x30]; add r12, r11; cmp r12, [r14+0x38]; ja 2f
///           mov r11, r12; mov r12, [rsp+8]; add rsp, 0x18; ret
///        2: mov [rsp], rdi; mov [rsp+0x10], rax; call main.flush
///           mov rdi, [rsp]; mov rax, [rsp+0x10]; jmp 1b
///   runtime.gcWriteBarrier2: mov r11d, 16; jmp gcWriteBarrier
///   main.h: push rbp; mov rbp, rsp; call [rdx]; pop rbp; ret
/// \p CallsRecovered adds main.h, which calls through a func value, as most
/// Go images do; recovery binds the calls of the image without it too, as
/// those of every run with a call of Go code.
/// \p FlushRecorded gives main.flush the pclntab record that says it takes
/// no stack argument.  \p FlushTakesStackWord makes it Go assembly that
/// takes a word on the stack instead (`mov rax, [rsp+8]; ret`), which the
/// body's store at [rsp] then passes.
struct GoBufferBarrierC {
  std::string Body;
  std::string Entry;
  std::vector<uint64_t> BodyParameters;
  /// The bytes of R11 the body's summary reads at its entry.
  unsigned BodyReadsOfBufferRegister = 0;
};

/// What a PE's function table says of the shared body: nothing, as in an
/// ELF; unwind data alone, as Go's linker writes; or a personality too.
enum class BarrierUnwind { None, Data, Personality };

GoBufferBarrierC
liftGoBufferBarrier(bool CallsRecovered, bool FlushRecorded,
                    bool FlushTakesStackWord = false,
                    bool ReloadSkipsItsSave = false,
                    BarrierUnwind Unwind = BarrierUnwind::None) {
  constexpr va_t Flush = kTextVA + 0x80;
  constexpr va_t Body = kTextVA + 0x100;
  constexpr va_t Entry = kTextVA + 0x200;
  constexpr va_t Other = kTextVA + 0x280;
  GoTestImage T(Arch::X64);
  if (FlushTakesStackWord)
    T.writeText(Flush, {0x48, 0x8B, 0x44, 0x24, 0x08, 0xC3});
  else
    T.writeText(Flush, {0xC3});
  std::vector<uint8_t> Code = {
      0x48, 0x83, 0xEC, 0x18, 0x4C, 0x89, 0x64, 0x24, 0x08, 0x4D, 0x8B,
      0x66, 0x30, 0x4D, 0x01, 0xDC, 0x4D, 0x3B, 0x66, 0x38, 0x77, 0x0D,
      0x4D, 0x89, 0xE3, 0x4C, 0x8B, 0x64, 0x24, 0x08, 0x48, 0x83, 0xC4,
      0x18, 0xC3, 0x48, 0x89, 0x3C, 0x24, 0x48, 0x89, 0x44, 0x24, 0x10};
  // A slow path that may skip its saves: test r11, r11; je <the reloads>.
  if (ReloadSkipsItsSave)
    Code.insert(Code.end() - 9, {0x4D, 0x85, 0xDB, 0x74, 0x0E});
  const std::vector<uint8_t> Call = makeCall(Body + Code.size(), Flush);
  Code.insert(Code.end(), Call.begin(), Call.end());
  Code.insert(Code.end(), {0x48, 0x8B, 0x3C, 0x24, 0x48, 0x8B, 0x44, 0x24, 0x10,
                           0xEB, uint8_t(ReloadSkipsItsSave ? 0xC8 : 0xCD)});
  T.writeText(Body, Code);
  std::vector<uint8_t> Numbered = {0x41, 0xBB, 0x10, 0x00, 0x00, 0x00, 0xE9};
  const auto Jump = static_cast<uint32_t>(Body - (Entry + 11));
  for (unsigned Shift = 0; Shift < 32; Shift += 8)
    Numbered.push_back(static_cast<uint8_t>(Jump >> Shift));
  T.writeText(Entry, Numbered);
  std::vector<std::pair<va_t, const char *>> Functions = {
      {Flush, "main.flush"},
      {Body, "gcWriteBarrier"},
      {Entry, "runtime.gcWriteBarrier2"}};
  GoModuleInfo Module;
  Module.PclnTabMagic = 0xFFFFFFF1u;
  Module.RegisterArguments = true;
  Module.GoCode = {{kTextVA, kTextVA + 0x400}};
  Module.GoAssembly = {{Body, Body + 0x40}, {Entry, Entry + 0x10}};
  Module.FrameArguments = {{Flush}, {Body}, {Entry}};
  if (FlushRecorded)
    Module.FrameArguments.front() = {Flush, 0, 0};
  if (FlushTakesStackWord) {
    Module.GoAssembly.insert(Module.GoAssembly.begin(), {Flush, Flush + 0x10});
    Module.FrameArguments.front() = {Flush, 8};
  }
  if (CallsRecovered) {
    T.writeText(Other, {0x55, 0x48, 0x89, 0xE5, 0xFF, 0x12, 0x5D, 0xC3});
    Functions.push_back({Other, "main.h"});
    Module.FrameArguments.push_back({Other, 0, 0});
  }
  for (const auto &[Address, Name] : Functions) {
    Symbol Function = Symbol::makeFunc(Address);
    Function.Name = Name;
    T.Img.Symbols.push_back(std::move(Function));
  }
  GoWriteBarrier Shared;
  Shared.Entry = Body;
  Shared.Kind = GoWriteBarrierKind::BufferBody;
  GoWriteBarrier Entered;
  Entered.Entry = Entry;
  Entered.Kind = GoWriteBarrierKind::Buffer;
  Module.WriteBarriers = {Shared, Entered};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  if (Unwind != BarrierUnwind::None) {
    T.Img.Format = BinaryFormat::COFF;
    ExceptionFunction Record;
    Record.FunctionEntry = Body;
    Record.CodeRange = {Body, Body + Code.size()};
    Record.Kind = RuntimeFunctionKind::Primary;
    Record.Encoding = ExceptionEncoding::X64UnwindV1;
    Record.UnwindVersion = 1;
    if (Unwind == BarrierUnwind::Personality)
      Record.Personality = ExceptionPersonality::Unknown;
    T.Img.ExceptionMetadata.Functions.push_back(std::move(Record));
    T.Img.ExceptionMetadata.rebuildIndex();
  }
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  llvm::LLVMContext Context;
  const PipelineResult R = Pipeline().run(T.Img, Context, Options);
  GoBufferBarrierC C;
  EXPECT_TRUE(R.Success) << R.Error;
  EXPECT_EQ(R.MedIRVerifierFailures, 0u);
  if (!R.Success)
    return C;
  C.Body = highCBody(R, T.Img, "gcWriteBarrier");
  C.Entry = highCBody(R, T.Img, "runtime_gcWriteBarrier2");
  for (const MedFunc &F : R.MedFuncs)
    if (F.Entry == Body)
      for (const MedVar &Parameter : F.Params)
        C.BodyParameters.push_back(Parameter.RegOff);
  if (const auto Reads = R.CallEntryReadGPRs.find(Body);
      Reads != R.CallEntryReadGPRs.end())
    C.BodyReadsOfBufferRegister = Reads->second[x86reg::R11 / 8];
  return C;
}
} // namespace

TEST(GoWriteBarriers, TheSharedBodyTakesAndReturnsItsBufferRegister) {
  // Go 1.21's numbered barriers load the bytes of buffer space they want
  // into R11 and jump to a body they share, `gcWriteBarrier`, which returns
  // that space in R11, reads the goroutine in R14 and keeps every other
  // register: its slow path saves them round the flush it calls.  Read by
  // System V's positions, the body took the registers it saves as six
  // parameters, each numbered barrier passed it six unknown values, and the
  // body returned an RAX it never set.  The saves and reloads are no source
  // statements where the flush is known to take no word on the stack: call
  // recovery binds its arguments, in an image with a call through a func
  // value or without one, whether or not its pclntab record gives it none.
  for (const bool Indirect : {true, false})
    for (const bool Recorded : {true, false}) {
      SCOPED_TRACE(testing::Message()
                   << "indirect=" << Indirect << " recorded=" << Recorded);
      const GoBufferBarrierC C = liftGoBufferBarrier(Indirect, Recorded);
      EXPECT_EQ(C.BodyParameters, std::vector<uint64_t>{x86reg::R11});
      EXPECT_EQ(C.BodyReadsOfBufferRegister, 8u);
      EXPECT_NE(C.Body.find("+ (uint64_t)arg0"), std::string::npos) << C.Body;
      EXPECT_NE(C.Body.find("%%r14"), std::string::npos) << C.Body;
      EXPECT_EQ(C.Body.find("__builtin_trap"), std::string::npos) << C.Body;
      EXPECT_EQ(C.Body.find("var_"), std::string::npos) << C.Body;
      EXPECT_EQ(C.Body.find("_known"), std::string::npos) << C.Body;
      EXPECT_NE(C.Entry.find("return gcWriteBarrier(0x10);"), std::string::npos)
          << C.Entry;
    }
}

TEST(GoWriteBarriers, AReloadNoSaveRanBeforeStays) {
  // A reload reads what its save stored only where the save ran first.  A
  // slow path that may skip its saves (`test r11, r11; je <the reloads>`)
  // reloads bytes nothing wrote the first time round: neither the saves nor
  // the reloads are a kept register's, and all four stay.
  for (const bool Recovered : {true, false}) {
    SCOPED_TRACE(testing::Message() << "recovered=" << Recovered);
    const GoBufferBarrierC C =
        liftGoBufferBarrier(Recovered, true, false, true);
    EXPECT_NE(C.Body.find("var_m18 = "), std::string::npos) << C.Body;
    EXPECT_NE(C.Body.find("= var_m18;"), std::string::npos) << C.Body;
    EXPECT_NE(C.Body.find("var_m8 = "), std::string::npos) << C.Body;
    EXPECT_NE(C.Body.find("= var_m8;"), std::string::npos) << C.Body;
    EXPECT_NE(C.Body.find("main_flush();"), std::string::npos) << C.Body;
  }
}

TEST(GoWriteBarriers, UnwindDataAloneEntersNoBlock) {
  // Go's linker gives every function of a PE unwind data and no personality.
  // Such data enters no block of the body, so its saves are still a kept
  // register's; a personality's dispatcher may read what the body saved, so
  // there the saves and reloads stay.
  const GoBufferBarrierC Plain =
      liftGoBufferBarrier(true, true, false, false, BarrierUnwind::Data);
  EXPECT_EQ(Plain.BodyParameters, std::vector<uint64_t>{x86reg::R11});
  EXPECT_NE(Plain.Body.find("main_flush();"), std::string::npos) << Plain.Body;
  EXPECT_EQ(Plain.Body.find("__builtin_trap"), std::string::npos) << Plain.Body;
  EXPECT_EQ(Plain.Body.find("var_"), std::string::npos) << Plain.Body;
  const GoBufferBarrierC Dispatched =
      liftGoBufferBarrier(true, true, false, false, BarrierUnwind::Personality);
  EXPECT_NE(Dispatched.Body.find("var_m18 = "), std::string::npos)
      << Dispatched.Body;
  EXPECT_NE(Dispatched.Body.find("= var_m18;"), std::string::npos)
      << Dispatched.Body;
  EXPECT_NE(Dispatched.Body.find("var_m8 = "), std::string::npos)
      << Dispatched.Body;
  EXPECT_NE(Dispatched.Body.find("= var_m8;"), std::string::npos)
      << Dispatched.Body;
}

TEST(GoWriteBarriers, KeepEveryRegisterButTheirResultAndArm64sTemporary) {
  for (const GoWriteBarrierKind Kind :
       {GoWriteBarrierKind::Store, GoWriteBarrierKind::Buffer,
        GoWriteBarrierKind::BufferBody}) {
    SCOPED_TRACE(static_cast<int>(Kind));
    const bool ReturnsSpace = Kind != GoWriteBarrierKind::Store;
    for (const uint64_t Register :
         {x86reg::RAX, x86reg::RDI, x86reg::RBP, x86reg::R10, x86reg::R15})
      EXPECT_TRUE(goWriteBarrierKeeps(Arch::X64, Kind, Register)) << Register;
    EXPECT_FALSE(goWriteBarrierKeeps(Arch::X64, Kind, x86reg::RSP));
    EXPECT_EQ(goWriteBarrierKeeps(Arch::X64, Kind, x86reg::R11), !ReturnsSpace);
    // No part of a register and nothing past R15 is one it keeps.
    EXPECT_FALSE(goWriteBarrierKeeps(Arch::X64, Kind, x86reg::RAX + 1));
    EXPECT_FALSE(goWriteBarrierKeeps(Arch::X64, Kind, x86reg::R15 + 8));
    for (const uint64_t Register :
         {a64reg::X0, a64reg::X15, a64reg::X16, a64reg::X19, a64reg::X26,
          a64reg::X28, a64reg::X29, a64reg::X30})
      EXPECT_TRUE(goWriteBarrierKeeps(Arch::AArch64, Kind, Register))
          << Register;
    EXPECT_FALSE(goWriteBarrierKeeps(Arch::AArch64, Kind, a64reg::X27));
    EXPECT_FALSE(goWriteBarrierKeeps(Arch::AArch64, Kind, a64reg::SP));
    EXPECT_EQ(goWriteBarrierKeeps(Arch::AArch64, Kind, a64reg::X25),
              !ReturnsSpace);
    EXPECT_FALSE(goWriteBarrierKeeps(Arch::X86, Kind, x86reg::RAX));
  }
}

TEST(GoWriteBarriers, TheSharedBodyKeepsArm64sUpperRegisters) {
  // arm64's body keeps X19 to X26 too, which the call summaries track no
  // family of, and saves them in pairs round the flush.  The pair of the
  // register it takes and returns in (X25, X26) is saved there as well: the
  // save of X26 is a kept register's, and that of X25, the bytes wanted,
  // stays with its reload.
  //   main.flush: ret
  //   gcWriteBarrier: str x30, [sp, #-64]!; stp x0, x1, [sp, #48]
  //     retry: ldr x0, [x28, #0x30]; ldr x1, [x0, #0x10]; ldr x27, [x0, #0x18]
  //            add x1, x1, x25; cmp x1, x27; b.hi flush; str x1, [x0, #0x10]
  //            sub x25, x1, x25; ldp x0, x1, [sp, #48]; ldr x30, [sp], #64
  //            ret
  //     flush: stp x19, x20, [sp, #16]; stp x25, x26, [sp, #32]
  //            bl main.flush; ldp x19, x20, [sp, #16]
  //            ldp x25, x26, [sp, #32]; b retry
  //   runtime.gcWriteBarrier2: mov x25, #16; b gcWriteBarrier
  constexpr va_t Flush = kTextVA + 0x80;
  constexpr va_t Body = kTextVA + 0x100;
  constexpr va_t Entry = kTextVA + 0x200;
  GoTestImage T(Arch::AArch64);
  T.writeText(Flush, words({0xD65F03C0}));
  const uint32_t Call =
      0x94000000u | (((Flush - (Body + 0x3C)) >> 2) & 0x3FFFFFFu);
  T.writeText(Body,
              words({0xF81C0FFE, 0xA90307E0, 0xF9401B80, 0xF9400801, 0xF9400C1B,
                     0x8B190021, 0xEB1B003F, 0x540000C8, 0xF9000801, 0xCB190039,
                     0xA94307E0, 0xF84407FE, 0xD65F03C0, 0xA90153F3, 0xA9026BF9,
                     Call, 0xA94153F3, 0xA9426BF9, 0x17FFFFF0}));
  const uint32_t Jump =
      0x14000000u | (((Body - (Entry + 4)) >> 2) & 0x3FFFFFFu);
  T.writeText(Entry, words({0xD2800219, Jump}));
  GoModuleInfo Module;
  Module.PclnTabMagic = 0xFFFFFFF1u;
  Module.RegisterArguments = true;
  Module.GoCode = {{kTextVA, kTextVA + 0x400}};
  Module.GoAssembly = {{Body, Body + 0x4C}, {Entry, Entry + 8}};
  Module.FrameArguments = {{Flush, 0, 0}, {Body}, {Entry}};
  GoWriteBarrier Shared;
  Shared.Entry = Body;
  Shared.Kind = GoWriteBarrierKind::BufferBody;
  GoWriteBarrier Entered;
  Entered.Entry = Entry;
  Entered.Kind = GoWriteBarrierKind::Buffer;
  Module.WriteBarriers = {Shared, Entered};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  for (const auto &[Address, Name] :
       {std::pair{Flush, "main.flush"}, std::pair{Body, "gcWriteBarrier"},
        std::pair{Entry, "runtime.gcWriteBarrier2"}}) {
    Symbol Function = Symbol::makeFunc(Address);
    Function.Name = Name;
    T.Img.Symbols.push_back(std::move(Function));
  }
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  llvm::LLVMContext Context;
  const PipelineResult R = Pipeline().run(T.Img, Context, Options);
  ASSERT_TRUE(R.Success) << R.Error;
  EXPECT_EQ(R.MedIRVerifierFailures, 0u);
  const std::string Text = highCBody(R, T.Img, "gcWriteBarrier");
  EXPECT_EQ(Text.find("__builtin_trap"), std::string::npos) << Text;
  // X19 and X20 at [sp, #16] and X26 at [sp, #40] ...
  EXPECT_EQ(Text.find("var_m30"), std::string::npos) << Text;
  EXPECT_EQ(Text.find("var_m28"), std::string::npos) << Text;
  EXPECT_EQ(Text.find("var_m18"), std::string::npos) << Text;
  // ... and X25 at [sp, #32].
  EXPECT_NE(Text.find("var_m20 = "), std::string::npos) << Text;
  EXPECT_NE(Text.find("= var_m20;"), std::string::npos) << Text;
  EXPECT_NE(Text.find("main_flush();"), std::string::npos) << Text;
  EXPECT_NE(highCBody(R, T.Img, "runtime_gcWriteBarrier2")
                .find("return gcWriteBarrier(0x10);"),
            std::string::npos);
}

TEST(GoWriteBarriers, AStackArgumentIsNoKeptRegister) {
  // Where the routine the slow path calls takes a word on the stack, the
  // store at [rsp] passes it and the reload reads what the routine left
  // there: both stay, though the value goes round the loop as a kept
  // register's does.  The store at [rsp+0x10], no argument, still goes.
  const GoBufferBarrierC C = liftGoBufferBarrier(true, true, true);
  EXPECT_NE(C.Body.find("var_m18 = "), std::string::npos) << C.Body;
  EXPECT_NE(C.Body.find("= var_m18;"), std::string::npos) << C.Body;
  EXPECT_NE(C.Body.find("main_flush(("), std::string::npos) << C.Body;
  EXPECT_EQ(C.Body.find("var_m8"), std::string::npos) << C.Body;
}

TEST(GoInternalABI, ARunWithoutIndirectCallsRecoversItsGoCalls) {
  // A call of Go code passes its arguments in Go's registers, which call
  // recovery reads in the callee's order.  HighIR's own collection numbers a
  // platform's registers, System V's six: a run with no indirect call and no
  // stack-ABI callee skipped recovery, as the run of one function a user
  // opens can, and a call that sets seven registers passed six and an unknown
  // value.
  //   main.seven: add rax, rbx; add rax, rcx; add rax, rdi; add rax, rsi
  //               add rax, r8; add rax, r9; ret
  //   main.user: mov eax, 1; mov ebx, 2; mov ecx, 3; mov edi, 4; mov esi, 5
  //              mov r8d, 6; mov r9d, 7; call main.seven; ret
  constexpr va_t Seven = kTextVA + 0x100;
  constexpr va_t User = kTextVA + 0x180;
  for (const BinaryFormat Format : {BinaryFormat::ELF, BinaryFormat::COFF}) {
    SCOPED_TRACE(static_cast<unsigned>(Format));
    GoTestImage T(Arch::X64);
    T.Img.Format = Format;
    T.writeText(Seven,
                {0x48, 0x01, 0xD8, 0x48, 0x01, 0xC8, 0x48, 0x01, 0xF8, 0x48,
                 0x01, 0xF0, 0x4C, 0x01, 0xC0, 0x4C, 0x01, 0xC8, 0xC3});
    std::vector<uint8_t> Body = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xBB, 0x02, 0x00,
                                 0x00, 0x00, 0xB9, 0x03, 0x00, 0x00, 0x00, 0xBF,
                                 0x04, 0x00, 0x00, 0x00, 0xBE, 0x05, 0x00, 0x00,
                                 0x00, 0x41, 0xB8, 0x06, 0x00, 0x00, 0x00, 0x41,
                                 0xB9, 0x07, 0x00, 0x00, 0x00};
    const std::vector<uint8_t> Call = makeCall(User + Body.size(), Seven);
    Body.insert(Body.end(), Call.begin(), Call.end());
    Body.push_back(0xC3);
    T.writeText(User, Body);
    for (const auto &[Address, Name] :
         {std::pair{Seven, "main.seven"}, std::pair{User, "main.user"}}) {
      Symbol Function = Symbol::makeFunc(Address);
      Function.Name = Name;
      T.Img.Symbols.push_back(std::move(Function));
    }
    GoModuleInfo Module;
    Module.PclnTabMagic = 0xFFFFFFF1u;
    Module.RegisterArguments = true;
    Module.GoCode = {{kTextVA, kTextVA + 0x400}};
    Module.FrameArguments = {{Seven, 56}, {User, 0}};
    T.Img.ExceptionMetadata.GoModule = std::move(Module);
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    llvm::LLVMContext Context;
    const PipelineResult R = Pipeline().run(T.Img, Context, Options);
    ASSERT_TRUE(R.Success) << R.Error;
    const std::string Calling = highCBody(R, T.Img, "main_user");
    EXPECT_NE(Calling.find("main_seven(1, 2, 3, 4, 5, 6, 7);"),
              std::string::npos)
        << Calling;
  }
}

TEST(GoInternalABI, ACallPassesGosNinthRegister) {
  // Go's internal ABI passes nine integer arguments in registers on x86-64,
  // R11 the last.  The operands a callee's summary names were counted over
  // the slots tracked for a dispatcher or a variadic call, eight: a call of
  // `runtime.concatstring4`, which takes a buffer and four strings, passed
  // eight arguments and an unknown value, and the move that set R11 was
  // dead.
  //   main.nine: add rax, rbx; add rax, rcx; add rax, rdi; add rax, rsi
  //              add rax, r8; add rax, r9; add rax, r10; add rax, r11; ret
  //   main.user: mov eax, 1; ...; mov r10d, 8; mov r11d, 9; call main.nine
  //              ret
  constexpr va_t Nine = kTextVA + 0x100;
  constexpr va_t User = kTextVA + 0x180;
  for (const BinaryFormat Format : {BinaryFormat::ELF, BinaryFormat::COFF}) {
    SCOPED_TRACE(static_cast<unsigned>(Format));
    GoTestImage T(Arch::X64);
    T.Img.Format = Format;
    T.writeText(Nine, {0x48, 0x01, 0xD8, 0x48, 0x01, 0xC8, 0x48, 0x01, 0xF8,
                       0x48, 0x01, 0xF0, 0x4C, 0x01, 0xC0, 0x4C, 0x01, 0xC8,
                       0x4C, 0x01, 0xD0, 0x4C, 0x01, 0xD8, 0xC3});
    std::vector<uint8_t> Body = {
        0xB8, 0x01, 0x00, 0x00, 0x00, 0xBB, 0x02, 0x00, 0x00, 0x00,
        0xB9, 0x03, 0x00, 0x00, 0x00, 0xBF, 0x04, 0x00, 0x00, 0x00,
        0xBE, 0x05, 0x00, 0x00, 0x00, 0x41, 0xB8, 0x06, 0x00, 0x00,
        0x00, 0x41, 0xB9, 0x07, 0x00, 0x00, 0x00, 0x41, 0xBA, 0x08,
        0x00, 0x00, 0x00, 0x41, 0xBB, 0x09, 0x00, 0x00, 0x00};
    const std::vector<uint8_t> Call = makeCall(User + Body.size(), Nine);
    Body.insert(Body.end(), Call.begin(), Call.end());
    Body.push_back(0xC3);
    T.writeText(User, Body);
    for (const auto &[Address, Name] :
         {std::pair{Nine, "main.nine"}, std::pair{User, "main.user"}}) {
      Symbol Function = Symbol::makeFunc(Address);
      Function.Name = Name;
      T.Img.Symbols.push_back(std::move(Function));
    }
    GoModuleInfo Module;
    Module.PclnTabMagic = 0xFFFFFFF1u;
    Module.RegisterArguments = true;
    Module.GoCode = {{kTextVA, kTextVA + 0x400}};
    Module.FrameArguments = {{Nine, 72}, {User, 0}};
    T.Img.ExceptionMetadata.GoModule = std::move(Module);
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    llvm::LLVMContext Context;
    const PipelineResult R = Pipeline().run(T.Img, Context, Options);
    ASSERT_TRUE(R.Success) << R.Error;
    const std::string Calling = highCBody(R, T.Img, "main_user");
    EXPECT_NE(Calling.find("main_nine(1, 2, 3, 4, 5, 6, 7, 8, 9);"),
              std::string::npos)
        << Calling;
  }
}

TEST(GoInternalABI, ACallKeepsTheSetupOfEveryGoRegister) {
  // Where a call takes its arguments from the setup its caller writes, with
  // no summary to bind them (arm64), the moves that set its argument
  // registers stay live until call recovery reads them, and the registers
  // kept were the platform's: X0 to X7.  Go passes sixteen integer
  // arguments in registers there.  `strconv.formatDigits` shifts its own into
  // place for `fmtE` (`mov x9, x10; mov x10, x11`): both moves were deleted
  // as dead, and the call passed what X9 and X10 held on entry, two of the
  // caller's other parameters.  The setup of a call of anything but code Go
  // compiled is as dead as before.
  constexpr va_t Callee = kTextVA + 0x100;
  for (const bool GoCallee : {true, false}) {
    SCOPED_TRACE(GoCallee);
    GoTestImage T(Arch::AArch64);
    GoModuleInfo Module;
    Module.PclnTabMagic = 0xFFFFFFF1u;
    Module.RegisterArguments = true;
    if (GoCallee)
      Module.GoCode = {{kTextVA, kTextVA + 0x400}};
    T.Img.ExceptionMetadata.GoModule = std::move(Module);
    const auto Register = [](uint64_t Offset, int Id, int Version) {
      MedVar Value;
      Value.Kind = MedVar::Reg;
      Value.TheArch = Arch::AArch64;
      Value.Id = Id;
      Value.SSAVer = Version;
      Value.Size = 8;
      Value.RegOff = Offset;
      return Value;
    };
    MedFunc Func;
    Func.Entry = kTextVA + 0x200;
    Func.Blocks.resize(1);
    MedBlock &Block = Func.Blocks.front();
    Block.Id = 0;
    MedOp Move;
    Move.Opcode = NdOp::COPY;
    Move.Output = Register(a64reg::X9, 9, 1);
    Move.addInput(Register(a64reg::X10, 10, 0));
    Block.Ops.push_back(Move);
    MedOp Call;
    Call.Opcode = NdOp::CALL;
    Call.addInput(MedVar::makeConst(Callee, 8));
    Block.Ops.push_back(Call);
    LowToMedConverter Converter;
    Converter.setBinaryImage(&T.Img);
    Converter.runRegisterDce(Func, Arch::AArch64);
    EXPECT_EQ(
        llvm::any_of(Func.Blocks.front().Ops,
                     [](const MedOp &Op) { return Op.Opcode == NdOp::COPY; }),
        GoCallee);
  }
}

TEST(GoInternalABI, ACallPassesTheArgumentsGoAssignsTheStack) {
  // Go's internal ABI assigns an argument the stack once its registers are
  // taken, or where those left cannot hold it whole, in words above the
  // return address, where the stack ABI puts every argument: Windows' home
  // space and a platform's register count have no part in it.  Read by the
  // platform's layout, such a function took a placeholder for each of System
  // V's six registers ahead of its own, which a lifted call, passing Go's
  // registers and the stack words in order, bound to the wrong parameters; on
  // Windows the words were locals nothing wrote, and no call passed them.
  //   main.eleven: nine register arguments, then add rax, [rsp+8]
  //                add rax, [rsp+0x10]; ret
  //   main.three: add rax, [rsp+8]; add rax, [rsp+0x10]; ret
  //   main.more: sub rsp, 0x18; mov qword [rsp], 10; mov qword [rsp+8], 11
  //              mov eax, 1; ...; mov r11d, 9; call main.eleven
  //              add rsp, 0x18; ret
  //   main.few: the same with RAX alone set, calling main.three
  //   main.wrap: jmp main.eleven
  constexpr va_t Eleven = kTextVA + 0x100;
  constexpr va_t Three = kTextVA + 0x140;
  constexpr va_t More = kTextVA + 0x200;
  constexpr va_t Few = kTextVA + 0x280;
  constexpr va_t Wrap = kTextVA + 0x300;
  const std::vector<uint8_t> Sum = {
      0x48, 0x01, 0xD8, 0x48, 0x01, 0xC8, 0x48, 0x01, 0xF8, 0x48, 0x01, 0xF0,
      0x4C, 0x01, 0xC0, 0x4C, 0x01, 0xC8, 0x4C, 0x01, 0xD0, 0x4C, 0x01, 0xD8};
  const std::vector<uint8_t> Stack = {0x48, 0x03, 0x44, 0x24, 0x08, 0x48,
                                      0x03, 0x44, 0x24, 0x10, 0xC3};
  const std::vector<uint8_t> Set = {
      0xB8, 0x01, 0x00, 0x00, 0x00, 0xBB, 0x02, 0x00, 0x00, 0x00,
      0xB9, 0x03, 0x00, 0x00, 0x00, 0xBF, 0x04, 0x00, 0x00, 0x00,
      0xBE, 0x05, 0x00, 0x00, 0x00, 0x41, 0xB8, 0x06, 0x00, 0x00,
      0x00, 0x41, 0xB9, 0x07, 0x00, 0x00, 0x00, 0x41, 0xBA, 0x08,
      0x00, 0x00, 0x00, 0x41, 0xBB, 0x09, 0x00, 0x00, 0x00};
  const std::vector<uint8_t> Push = {0x48, 0x83, 0xEC, 0x18, 0x48, 0xC7, 0x04,
                                     0x24, 0x0A, 0x00, 0x00, 0x00, 0x48, 0xC7,
                                     0x44, 0x24, 0x08, 0x0B, 0x00, 0x00, 0x00};
  for (const BinaryFormat Format : {BinaryFormat::ELF, BinaryFormat::COFF})
    for (const bool Lift : {false, true}) {
      SCOPED_TRACE(testing::Message() << "pe=" << (Format == BinaryFormat::COFF)
                                      << " lift=" << Lift);
      GoTestImage T(Arch::X64);
      T.Img.Format = Format;
      std::vector<uint8_t> Body = Sum;
      Body.insert(Body.end(), Stack.begin(), Stack.end());
      T.writeText(Eleven, Body);
      T.writeText(Three, Stack);
      Body = Push;
      Body.insert(Body.end(), Set.begin(), Set.end());
      std::vector<uint8_t> Call = makeCall(More + Body.size(), Eleven);
      Body.insert(Body.end(), Call.begin(), Call.end());
      Body.insert(Body.end(), {0x48, 0x83, 0xC4, 0x18, 0xC3});
      T.writeText(More, Body);
      Body = Push;
      Body.insert(Body.end(), {0xB8, 0x01, 0x00, 0x00, 0x00});
      Call = makeCall(Few + Body.size(), Three);
      Body.insert(Body.end(), Call.begin(), Call.end());
      Body.insert(Body.end(), {0x48, 0x83, 0xC4, 0x18, 0xC3});
      T.writeText(Few, Body);
      Body = {0xE9};
      const auto Jump = static_cast<uint32_t>(Eleven - (Wrap + 5));
      for (unsigned Shift = 0; Shift < 32; Shift += 8)
        Body.push_back(static_cast<uint8_t>(Jump >> Shift));
      T.writeText(Wrap, Body);
      for (const auto &[Address, Name] :
           {std::pair{Eleven, "main.eleven"}, std::pair{Three, "main.three"},
            std::pair{More, "main.more"}, std::pair{Few, "main.few"},
            std::pair{Wrap, "main.wrap"}}) {
        Symbol Function = Symbol::makeFunc(Address);
        Function.Name = Name;
        T.Img.Symbols.push_back(std::move(Function));
      }
      GoModuleInfo Module;
      Module.PclnTabMagic = 0xFFFFFFF1u;
      Module.RegisterArguments = true;
      Module.GoCode = {{kTextVA, kTextVA + 0x400}};
      Module.FrameArguments = {
          {Eleven, 88}, {Three, 24}, {More, 0}, {Few, 0}, {Wrap, 88}};
      T.Img.ExceptionMetadata.GoModule = std::move(Module);
      PipelineOptions Options;
      Options.EmitDumpOutput = false;
      Options.LiftMode = Lift;
      llvm::LLVMContext Context;
      const PipelineResult R = Pipeline().run(T.Img, Context, Options);
      ASSERT_TRUE(R.Success) << R.Error;
      if (Lift) {
        // Each call passes what its callee declares, in the callee's order:
        // the lifted callers return the sums, 1 to 11 and 1 + 10 + 11, where
        // the misbound ones returned 45 and 0.
        ASSERT_TRUE(R.LlvmModule);
        for (const auto &[Caller, Sum] :
             {std::pair{"main.more", 66u}, std::pair{"main.few", 22u}}) {
          SCOPED_TRACE(Caller);
          const llvm::Function *Calling = R.LlvmModule->getFunction(Caller);
          ASSERT_TRUE(Calling);
          std::optional<uint64_t> Returned;
          for (const llvm::BasicBlock &Block : *Calling)
            if (const auto *Return =
                    llvm::dyn_cast<llvm::ReturnInst>(Block.getTerminator()))
              if (const auto *Value = llvm::dyn_cast_or_null<llvm::ConstantInt>(
                      Return->getReturnValue()))
                Returned = Value->getZExtValue();
          std::string Text;
          llvm::raw_string_ostream OS(Text);
          Calling->print(OS);
          EXPECT_EQ(Returned, std::optional<uint64_t>(Sum)) << Text;
        }
        for (const auto &[Name, Arguments] :
             {std::pair{"main.eleven", 11u}, std::pair{"main.three", 3u},
              std::pair{"main.wrap", 11u}}) {
          SCOPED_TRACE(Name);
          const llvm::Function *Taking = R.LlvmModule->getFunction(Name);
          ASSERT_TRUE(Taking);
          EXPECT_EQ(Taking->arg_size(), Arguments);
        }
        continue;
      }
      const std::string Taking = highCBody(R, T.Img, "main_eleven");
      EXPECT_NE(Taking.find("int64_t arg10) {"), std::string::npos) << Taking;
      EXPECT_EQ(Taking.find("var_"), std::string::npos) << Taking;
      const std::string Few3 = highCBody(R, T.Img, "main_three");
      EXPECT_NE(Few3.find("main_three(int64_t arg0, int64_t arg1, int64_t "
                          "arg2) {"),
                std::string::npos)
          << Few3;
      EXPECT_EQ(Few3.find("var_"), std::string::npos) << Few3;
      const std::string Passing = highCBody(R, T.Img, "main_more");
      EXPECT_NE(
          Passing.find("main_eleven(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0xB);"),
          std::string::npos)
          << Passing;
      const std::string PassingFew = highCBody(R, T.Img, "main_few");
      EXPECT_NE(PassingFew.find("main_three(1, 10, 0xB);"), std::string::npos)
          << PassingFew;
      const std::string Wrapping = highCBody(R, T.Img, "main_wrap");
      EXPECT_NE(Wrapping.find("int64_t arg10) {"), std::string::npos)
          << Wrapping;
      EXPECT_NE(Wrapping.find(", arg1, arg2, arg3, arg4, arg5, arg6, arg7, "
                              "arg8, arg9, arg10);"),
                std::string::npos)
          << Wrapping;
    }
}

TEST(GoInternalABI, AnArm64StackArgumentFollowsNoPlatformPosition) {
  // On arm64 the word at the stack pointer holds the caller's link register
  // and Go's stack arguments start above it; AAPCS64's start at the stack
  // pointer, past eight registers.  Read so, main.far's word was its tenth
  // argument: the call passed X1 to X7 to reach it, which main.near, setting
  // none of them, took as seven parameters of its own.
  //   main.far: ldr x16, [sp, #8]; add x0, x0, x16; ret
  //   main.near: str x30, [sp, #-0x20]!; mov x0, #1; mov x1, #7
  //              str x1, [sp, #8]; bl main.far; ldr x30, [sp], #0x20; ret
  constexpr va_t Far = kTextVA + 0x100;
  constexpr va_t Near = kTextVA + 0x180;
  GoTestImage T(Arch::AArch64);
  T.writeText(Far, words({0xF94007F0, 0x8B100000, 0xD65F03C0}));
  const va_t At = Near + 4 * 4;
  T.writeText(Near, words({0xF81E0FFE, 0xD2800020, 0xD28000E1, 0xF90007E1,
                           0x94000000u | static_cast<uint32_t>(
                                             ((Far - At) / 4) & 0x03FFFFFFu),
                           0xF84207FE, 0xD65F03C0}));
  for (const auto &[Address, Name] :
       {std::pair{Far, "main.far"}, std::pair{Near, "main.near"}}) {
    Symbol Function = Symbol::makeFunc(Address);
    Function.Name = Name;
    T.Img.Symbols.push_back(std::move(Function));
  }
  GoModuleInfo Module;
  Module.PclnTabMagic = 0xFFFFFFF1u;
  Module.RegisterArguments = true;
  Module.GoCode = {{kTextVA, kTextVA + 0x400}};
  Module.FrameArguments = {{Far, 16}, {Near, 0}};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  llvm::LLVMContext Context;
  const PipelineResult R = Pipeline().run(T.Img, Context, Options);
  ASSERT_TRUE(R.Success) << R.Error;
  for (const MedFunc &F : R.MedFuncs) {
    SCOPED_TRACE(F.Entry - kTextVA);
    EXPECT_EQ(F.Params.size(), F.Entry == Far ? 2u : 0u);
  }
  const std::string Taking = highCBody(R, T.Img, "main_far");
  EXPECT_NE(Taking.find("main_far(int64_t arg0, int64_t arg1) {"),
            std::string::npos)
      << Taking;
  const std::string Passing = highCBody(R, T.Img, "main_near");
  EXPECT_NE(Passing.find("main_near(void) {"), std::string::npos) << Passing;
  EXPECT_NE(Passing.find("main_far(1, 7);"), std::string::npos) << Passing;
}

TEST(GoStackABI, AnArgumentNarrowerThanAWordIsStillOne) {
  // A stack-ABI call passes no more words than its callee's record gives.
  // The bytes were divided by the word size, so a record of four bytes, one
  // int32, gave no word: `runtime.exit(code int32)`, assembly that reads its
  // code from the stack, took it from every arm64 caller as an unknown
  // value.
  //   main.exit: ldrsw x0, [sp, #8]; ret
  //   main.user: str x30, [sp, #-0x20]!; mov x0, #2; str w0, [sp, #8]
  //              bl main.exit; ldr x30, [sp], #0x20; ret
  constexpr va_t Exit = kTextVA + 0x100;
  constexpr va_t User = kTextVA + 0x180;
  GoTestImage T(Arch::AArch64);
  T.writeText(Exit, words({0xB9800BE0, 0xD65F03C0}));
  const va_t At = User + 3 * 4;
  T.writeText(User, words({0xF81E0FFE, 0xD2800040, 0xB9000BE0,
                           0x94000000u | static_cast<uint32_t>(
                                             ((Exit - At) / 4) & 0x03FFFFFFu),
                           0xF84207FE, 0xD65F03C0}));
  for (const auto &[Address, Name] :
       {std::pair{Exit, "main.exit"}, std::pair{User, "main.user"}}) {
    Symbol Function = Symbol::makeFunc(Address);
    Function.Name = Name;
    T.Img.Symbols.push_back(std::move(Function));
  }
  GoModuleInfo Module;
  Module.PclnTabMagic = 0xFFFFFFF1u;
  Module.RegisterArguments = true;
  Module.GoCode = {{kTextVA, kTextVA + 0x400}};
  Module.GoAssembly = {{Exit, Exit + 0x10}};
  Module.FrameArguments = {{Exit, 4}, {User, 0}};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  llvm::LLVMContext Context;
  const PipelineResult R = Pipeline().run(T.Img, Context, Options);
  ASSERT_TRUE(R.Success) << R.Error;
  const std::string Calling = highCBody(R, T.Img, "main_user");
  EXPECT_NE(Calling.find("main_exit(2);"), std::string::npos) << Calling;
  EXPECT_EQ(Calling.find("__builtin_trap"), std::string::npos) << Calling;
}

TEST(GoAssemblyOperands, ARoutineThatResumesSavedStateTakesWhatItReads) {
  // `gogo<>` takes the gobuf in BX and the goroutine in DX, loads the stack
  // pointer, the closure context and the frame pointer from the gobuf, and
  // jumps to the PC saved there.  A jump to code no summary describes passes
  // this routine's argument registers on, so what the routine reads at its
  // entry was unknown and it took no operand of its own: it read the gobuf
  // through a register with no definition, and `runtime.gogo`, which sets
  // both registers, passed two unknown values.  A jump that follows a load
  // of the stack pointer from memory resumes state saved elsewhere, on that
  // state's stack: it calls nothing and reads no register as the routine was
  // entered with it.  One that loads no stack pointer is a tail call still.
  //   gogo: mov r14, rdx; mov rsp, [rbx]; mov rdx, [rbx+0x18]
  //         mov rbp, [rbx+0x28]; mov qword [rbx], 0; mov rbx, [rbx+8]; jmp rbx
  //   runtime.gogo: mov rbx, [rsp+8]; mov rdx, [rbx+0x10]; jmp gogo
  //   main.hop: mov rbx, [rbx+8]; jmp rbx
  constexpr va_t Gogo = kTextVA + 0x100;
  constexpr va_t Entered = kTextVA + 0x180;
  constexpr va_t Hop = kTextVA + 0x200;
  for (const BinaryFormat Format : {BinaryFormat::ELF, BinaryFormat::COFF}) {
    SCOPED_TRACE(static_cast<unsigned>(Format));
    GoTestImage T(Arch::X64);
    T.Img.Format = Format;
    T.writeText(Gogo, {0x49, 0x89, 0xD6, 0x48, 0x8B, 0x23, 0x48, 0x8B, 0x53,
                       0x18, 0x48, 0x8B, 0x6B, 0x28, 0x48, 0xC7, 0x03, 0x00,
                       0x00, 0x00, 0x00, 0x48, 0x8B, 0x5B, 0x08, 0xFF, 0xE3});
    std::vector<uint8_t> Body = {0x48, 0x8B, 0x5C, 0x24, 0x08,
                                 0x48, 0x8B, 0x53, 0x10, 0xE9};
    const auto Jump = static_cast<uint32_t>(Gogo - (Entered + Body.size() + 4));
    for (unsigned Shift = 0; Shift < 32; Shift += 8)
      Body.push_back(static_cast<uint8_t>(Jump >> Shift));
    T.writeText(Entered, Body);
    T.writeText(Hop, {0x48, 0x8B, 0x5B, 0x08, 0xFF, 0xE3});
    for (const auto &[Address, Name] :
         {std::pair{Gogo, "gogo"}, std::pair{Entered, "runtime.gogo"},
          std::pair{Hop, "main.hop"}}) {
      Symbol Function = Symbol::makeFunc(Address);
      Function.Name = Name;
      T.Img.Symbols.push_back(std::move(Function));
    }
    GoModuleInfo Module;
    Module.PclnTabMagic = 0xFFFFFFF1u;
    Module.RegisterArguments = true;
    Module.GoCode = {{kTextVA, kTextVA + 0x400}};
    Module.GoAssembly = {
        {Gogo, Gogo + 0x20}, {Entered, Entered + 0x10}, {Hop, Hop + 0x10}};
    Module.FrameArguments = {{Gogo}, {Entered, 8}, {Hop}};
    T.Img.ExceptionMetadata.GoModule = std::move(Module);
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    llvm::LLVMContext Context;
    const PipelineResult R = Pipeline().run(T.Img, Context, Options);
    ASSERT_TRUE(R.Success) << R.Error;
    const auto &Operands = R.CallEntryIntegerArguments.get();
    const auto Taken = Operands.find(Gogo);
    ASSERT_NE(Taken, Operands.end());
    std::vector<uint64_t> Registers;
    for (const auto &Parameter : Taken->second.Parameters)
      Registers.push_back(Parameter.Register);
    EXPECT_EQ(Registers, (std::vector<uint64_t>{x86reg::RDX, x86reg::RBX}));
    EXPECT_EQ(Operands.count(Hop), 0u);
    const std::string Resuming = highCBody(R, T.Img, "gogo");
    EXPECT_EQ(Resuming.find("__builtin_trap"), std::string::npos) << Resuming;
    EXPECT_NE(Resuming.find(")();"), std::string::npos) << Resuming;
    const std::string Entering = highCBody(R, T.Img, "runtime_gogo");
    EXPECT_EQ(Entering.find("__builtin_trap"), std::string::npos) << Entering;
    EXPECT_NE(Entering.find("gogo(*(int64_t *)(arg0 + 0x10), "),
              std::string::npos)
        << Entering;
  }
}

TEST(GoInternalABI, AFunctionThatPassesItsFirstArgumentOnReadsIt) {
  // A function reads at its entry what a callee of it reads at its own,
  // where the function wrote the register nowhere before the call.  The
  // call's write of its result ended that read: the summary cleared the
  // result register after it joined the callee's reads, though the callee
  // reads what it was entered with before the call writes anything.  Go
  // passes its first argument in the register it returns its first result
  // in, so a function that passes RAX on as it came, as `runtime.chansend1`
  // jumps to `chansend`, read RBX and RCX and not RAX: it took RAX as a
  // parameter only by the call that forwards it, and passed on a register
  // with no definition.
  //   main.three: add rax, rbx; add rax, rcx; ret
  //   main.wrap: jmp main.three
  constexpr va_t Three = kTextVA + 0x100;
  constexpr va_t Wrap = kTextVA + 0x180;
  for (const BinaryFormat Format : {BinaryFormat::ELF, BinaryFormat::COFF}) {
    SCOPED_TRACE(static_cast<unsigned>(Format));
    GoTestImage T(Arch::X64);
    T.Img.Format = Format;
    T.writeText(Three, {0x48, 0x01, 0xD8, 0x48, 0x01, 0xC8, 0xC3});
    std::vector<uint8_t> Body = {0xE9};
    const auto Jump = static_cast<uint32_t>(Three - (Wrap + 5));
    for (unsigned Shift = 0; Shift < 32; Shift += 8)
      Body.push_back(static_cast<uint8_t>(Jump >> Shift));
    T.writeText(Wrap, Body);
    for (const auto &[Address, Name] :
         {std::pair{Three, "main.three"}, std::pair{Wrap, "main.wrap"}}) {
      Symbol Function = Symbol::makeFunc(Address);
      Function.Name = Name;
      T.Img.Symbols.push_back(std::move(Function));
    }
    GoModuleInfo Module;
    Module.PclnTabMagic = 0xFFFFFFF1u;
    Module.RegisterArguments = true;
    Module.GoCode = {{kTextVA, kTextVA + 0x400}};
    Module.FrameArguments = {{Three, 24}, {Wrap, 24}};
    T.Img.ExceptionMetadata.GoModule = std::move(Module);
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    llvm::LLVMContext Context;
    const PipelineResult R = Pipeline().run(T.Img, Context, Options);
    ASSERT_TRUE(R.Success) << R.Error;
    const auto Reads = R.CallEntryReadGPRs.find(Wrap);
    ASSERT_NE(Reads, R.CallEntryReadGPRs.end());
    for (const uint64_t Register : {x86reg::RAX, x86reg::RBX, x86reg::RCX})
      EXPECT_EQ(Reads->second[Register / 8], 8u) << Register;
    for (const MedFunc &F : R.MedFuncs)
      if (F.Entry == Wrap) {
        ASSERT_EQ(F.Params.size(), 3u);
        for (const MedVar &Parameter : F.Params)
          EXPECT_GE(Parameter.Id, 0) << Parameter.RegOff;
      }
    const std::string Wrapping = highCBody(R, T.Img, "main_wrap");
    EXPECT_NE(Wrapping.find("return main_three(arg0, arg1, arg2);"),
              std::string::npos)
        << Wrapping;
  }
}

TEST(GoInternalABI, AFuncValueSetBeforeTheCallsBlockIsStillOne) {
  // Go assembly passes a func value only what it sets in Go's registers.
  // `runtime.mcall` moves its fn into the context register at its entry and
  // calls through it two blocks later: the block of the call sets the
  // register nowhere, so the call was no func value call, took the platform's
  // positions, and what it passed on in RDI and RSI became parameters of the
  // routine, which every caller then passed unknown values for.  The register
  // holds what the one block that leads to the call left in it.
  //   main.mc: mov rdx, rax; test rbx, rbx; jne 1f; ret
  //         1: mov eax, 5; mov r12, [rdx]; call r12; ret
  //   main.user: lea rax, [rip+0]; mov ebx, 1; call main.mc; ret
  constexpr va_t Mc = kTextVA + 0x100;
  constexpr va_t User = kTextVA + 0x140;
  GoTestImage T(Arch::X64);
  T.writeText(Mc,
              {0x48, 0x89, 0xC2, 0x48, 0x85, 0xDB, 0x75, 0x01, 0xC3, 0xB8, 0x05,
               0x00, 0x00, 0x00, 0x4C, 0x8B, 0x22, 0x41, 0xFF, 0xD4, 0xC3});
  std::vector<uint8_t> Caller = {0x48, 0x8D, 0x05, 0x00, 0x00, 0x00,
                                 0x00, 0xBB, 0x01, 0x00, 0x00, 0x00};
  const std::vector<uint8_t> Call = makeCall(User + Caller.size(), Mc);
  Caller.insert(Caller.end(), Call.begin(), Call.end());
  Caller.push_back(0xC3);
  T.writeText(User, Caller);
  for (const auto &[Address, Name] :
       {std::pair{Mc, "main.mc"}, std::pair{User, "main.user"}}) {
    Symbol Function = Symbol::makeFunc(Address);
    Function.Name = Name;
    T.Img.Symbols.push_back(std::move(Function));
  }
  GoModuleInfo Module;
  Module.PclnTabMagic = 0xFFFFFFF1u;
  Module.RegisterArguments = true;
  Module.GoCode = {{kTextVA, kTextVA + 0x400}};
  Module.GoAssembly = {{Mc, Mc + 0x20}};
  Module.FrameArguments = {{Mc, 16, 2}, {User, 0, 0}};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  llvm::LLVMContext Context;
  const PipelineResult R = Pipeline().run(T.Img, Context, Options);
  ASSERT_TRUE(R.Success) << R.Error;
  std::vector<uint64_t> Registers;
  for (const MedFunc &F : R.MedFuncs)
    if (F.Entry == Mc)
      for (const MedVar &Parameter : F.Params)
        Registers.push_back(Parameter.RegOff);
  EXPECT_EQ(Registers, (std::vector<uint64_t>{x86reg::RAX, x86reg::RBX}));
  const std::string Routine = highCBody(R, T.Img, "main_mc");
  EXPECT_NE(Routine.find(")((uint64_t)5);"), std::string::npos) << Routine;
  const std::string Calling = highCBody(R, T.Img, "main_user");
  EXPECT_NE(Calling.find(", 1);"), std::string::npos) << Calling;
  EXPECT_EQ(Calling.find("__builtin_trap"), std::string::npos) << Calling;
}

TEST(GoScheduleCalls, ACallOfMcallResumesAtItsReturnAddress) {
  // `runtime.mcall` saves its caller's state, switches to the scheduler's
  // stack and calls a function that never returns, so its own code never
  // returns: here it ends in a call of a routine that loops for ever.  The
  // scheduler resumes its caller at the call's return address all the same.
  // Proved terminal, the call ended main.park, which lost its `return 7`,
  // and every function that calls main.park never returned either, as
  // `runtime.gopark` and each blocking operation above it did.
  //   runtime.throw: jmp runtime.throw
  //   runtime.mcall: mov rdx, rax; mov r12, [rdx]; call r12
  //                  call runtime.throw; ud2
  //   main.park: sub rsp, 8; lea rax, [rip+0]; call runtime.mcall
  //              mov eax, 7; add rsp, 8; ret
  //   main.wait: sub rsp, 8; call main.park; add eax, 1; add rsp, 8; ret
  constexpr va_t Throw = kTextVA + 0xC0;
  constexpr va_t Mcall = kTextVA + 0x100;
  constexpr va_t Park = kTextVA + 0x140;
  constexpr va_t Wait = kTextVA + 0x180;
  GoTestImage T(Arch::X64);
  T.writeText(Throw, {0xEB, 0xFE});
  std::vector<uint8_t> Code = {0x48, 0x89, 0xC2, 0x4C, 0x8B,
                               0x22, 0x41, 0xFF, 0xD4};
  std::vector<uint8_t> Call = makeCall(Mcall + Code.size(), Throw);
  Code.insert(Code.end(), Call.begin(), Call.end());
  Code.insert(Code.end(), {0x0F, 0x0B});
  T.writeText(Mcall, Code);
  Code = {0x48, 0x83, 0xEC, 0x08, 0x48, 0x8D, 0x05, 0x00, 0x00, 0x00, 0x00};
  Call = makeCall(Park + Code.size(), Mcall);
  Code.insert(Code.end(), Call.begin(), Call.end());
  Code.insert(Code.end(),
              {0xB8, 0x07, 0x00, 0x00, 0x00, 0x48, 0x83, 0xC4, 0x08, 0xC3});
  T.writeText(Park, Code);
  Code = {0x48, 0x83, 0xEC, 0x08};
  Call = makeCall(Wait + Code.size(), Park);
  Code.insert(Code.end(), Call.begin(), Call.end());
  Code.insert(Code.end(), {0x83, 0xC0, 0x01, 0x48, 0x83, 0xC4, 0x08, 0xC3});
  T.writeText(Wait, Code);
  for (const auto &[Address, Name] :
       {std::pair{Throw, "runtime.throw"}, std::pair{Mcall, "runtime.mcall"},
        std::pair{Park, "main.park"}, std::pair{Wait, "main.wait"}}) {
    Symbol Function = Symbol::makeFunc(Address);
    Function.Name = Name;
    T.Img.Symbols.push_back(std::move(Function));
  }
  GoModuleInfo Module;
  Module.PclnTabMagic = 0xFFFFFFF1u;
  Module.RegisterArguments = true;
  Module.GoCode = {{kTextVA, kTextVA + 0x400}};
  Module.GoAssembly = {{Mcall, Mcall + 0x10}};
  Module.FrameArguments = {
      {Throw, 0, 0}, {Mcall, 8, 1}, {Park, 0, 0}, {Wait, 0, 0}};
  Module.ScheduleCallEntries = {Mcall};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  llvm::LLVMContext Context;
  const PipelineResult R = Pipeline().run(T.Img, Context, Options);
  ASSERT_TRUE(R.Success) << R.Error;
  for (const MedFunc &F : R.MedFuncs) {
    SCOPED_TRACE(F.Entry - kTextVA);
    // Each function's own flag is what its body proves: the routine that
    // loops and `mcall` itself never return, their callers do.
    EXPECT_EQ(F.DoesNotReturn, F.Entry == Throw || F.Entry == Mcall);
  }
  // To C, `mcall` is no `_Noreturn` function: its callers go on.
  for (const HighFunc &F : R.HighFuncs) {
    SCOPED_TRACE(F.Entry - kTextVA);
    EXPECT_EQ(F.DoesNotReturn, F.Entry == Throw);
  }
  const std::string Parked = highCBody(R, T.Img, "main_park");
  EXPECT_NE(Parked.find("runtime_mcall("), std::string::npos) << Parked;
  EXPECT_NE(Parked.find("return 7;"), std::string::npos) << Parked;
  const std::string Waited = highCBody(R, T.Img, "main_wait");
  EXPECT_NE(Waited.find("main_park()"), std::string::npos) << Waited;
  EXPECT_NE(Waited.find("+ 1;"), std::string::npos) << Waited;
}

TEST(GoInternalABI, AssemblyThatAddressesThroughR14ReadsTheGoroutine) {
  // Go's internal ABI keeps the goroutine in R14, and x86-64 assembly that
  // Go code enters still finds it there, whatever ABI it declares: main.save
  // stores through the register as it was entered, as `runtime.mcall` saves
  // its caller's state in `g.sched`, so it reads the goroutine.  Assembly
  // the platform enters only saves R14 for its caller: main.host, which
  // stores it into a record it was handed, takes the register as an operand
  // of its own and reads no goroutine.
  //   main.save: mov [r14+0x38], rax; mov rbx, [r14+0x30]; mov rax, rbx; ret
  //   main.host: mov [rdi], r14; ret
  constexpr va_t Save = kTextVA + 0x100;
  constexpr va_t Host = kTextVA + 0x140;
  for (const BinaryFormat Format : {BinaryFormat::ELF, BinaryFormat::COFF}) {
    SCOPED_TRACE(static_cast<unsigned>(Format));
    GoTestImage T(Arch::X64);
    T.Img.Format = Format;
    T.writeText(Save, {0x49, 0x89, 0x46, 0x38, 0x49, 0x8B, 0x5E, 0x30, 0x48,
                       0x89, 0xD8, 0xC3});
    T.writeText(Host, {0x4C, 0x89, 0x37, 0xC3});
    for (const auto &[Address, Name] :
         {std::pair{Save, "main.save"}, std::pair{Host, "main.host"}}) {
      Symbol Function = Symbol::makeFunc(Address);
      Function.Name = Name;
      T.Img.Symbols.push_back(std::move(Function));
    }
    GoModuleInfo Module;
    Module.PclnTabMagic = 0xFFFFFFF1u;
    Module.RegisterArguments = true;
    Module.GoCode = {{kTextVA, kTextVA + 0x400}};
    Module.GoAssembly = {{Save, Save + 0x10}, {Host, Host + 0x10}};
    Module.FrameArguments = {{Save, 0}, {Host, 0}};
    T.Img.ExceptionMetadata.GoModule = std::move(Module);
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    llvm::LLVMContext Context;
    const PipelineResult R = Pipeline().run(T.Img, Context, Options);
    ASSERT_TRUE(R.Success) << R.Error;
    const std::string Saving = highCBody(R, T.Img, "main_save");
    EXPECT_NE(Saving.find("%%r14"), std::string::npos) << Saving;
    EXPECT_NE(Saving.find("(g + 0x38) = arg0;"), std::string::npos) << Saving;
    EXPECT_NE(Saving.find("(g + 0x30)"), std::string::npos) << Saving;
    EXPECT_EQ(Saving.find("__builtin_trap"), std::string::npos) << Saving;
    const std::string Entered = highCBody(R, T.Img, "main_host");
    EXPECT_EQ(Entered.find("%%r14"), std::string::npos) << Entered;
    EXPECT_NE(Entered.find("*(int64_t *)arg0 = arg1;"), std::string::npos)
        << Entered;
  }
}

TEST(GoInternalABI, AssemblyTakesFloatsInEveryGoRegister) {
  // Go's internal ABI passes floating arguments in X0 to X14, where System V
  // passes eight and Win64 four.  Assembly that takes operands in registers
  // of its own, as `runtime.spillArgs` takes the address in R12 and stores
  // every argument register through it, takes the floating ones in Go's:
  // X8 and X14 were registers with no definition.
  //   main.spill: mov [r12], rax; movq [r12+8], xmm0; movq [r12+0x10], xmm8
  //               movq [r12+0x18], xmm14; ret
  constexpr va_t Spill = kTextVA + 0x100;
  for (const BinaryFormat Format : {BinaryFormat::ELF, BinaryFormat::COFF}) {
    SCOPED_TRACE(static_cast<unsigned>(Format));
    GoTestImage T(Arch::X64);
    T.Img.Format = Format;
    T.writeText(Spill, {0x49, 0x89, 0x04, 0x24, 0x66, 0x41, 0x0F, 0xD6, 0x44,
                        0x24, 0x08, 0x66, 0x45, 0x0F, 0xD6, 0x44, 0x24, 0x10,
                        0x66, 0x45, 0x0F, 0xD6, 0x74, 0x24, 0x18, 0xC3});
    Symbol Function = Symbol::makeFunc(Spill);
    Function.Name = "main.spill";
    T.Img.Symbols.push_back(std::move(Function));
    GoModuleInfo Module;
    Module.PclnTabMagic = 0xFFFFFFF1u;
    Module.RegisterArguments = true;
    Module.GoCode = {{kTextVA, kTextVA + 0x400}};
    Module.GoAssembly = {{Spill, Spill + 0x20}};
    Module.FrameArguments = {{Spill, 0}};
    T.Img.ExceptionMetadata.GoModule = std::move(Module);
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    llvm::LLVMContext Context;
    const PipelineResult R = Pipeline().run(T.Img, Context, Options);
    ASSERT_TRUE(R.Success) << R.Error;
    std::vector<uint64_t> Registers;
    for (const MedFunc &F : R.MedFuncs)
      if (F.Entry == Spill)
        for (const MedVar &Parameter : F.Params)
          Registers.push_back(Parameter.RegOff);
    EXPECT_EQ(Registers, (std::vector<uint64_t>{
                             x86reg::RAX, x86reg::R12, x86reg::vectorReg(0),
                             x86reg::vectorReg(8), x86reg::vectorReg(14)}));
    const std::string Text = highCBody(R, T.Img, "main_spill");
    EXPECT_EQ(Text.find("__builtin_trap"), std::string::npos) << Text;
    EXPECT_NE(Text.find("(double)(arg3)"), std::string::npos) << Text;
    EXPECT_NE(Text.find("(double)(arg4)"), std::string::npos) << Text;
  }
}

TEST(GoInternalABI, AnARM64RoutineThatReadsGoRegistersTakesGoFloats) {
  // AAPCS64's integer positions are Go's first eight, so only a read of a
  // register past them tells Go's convention from the platform's: X9 carries
  // Go's tenth argument and nothing the platform passes.  main.spill reads
  // it, so the D8 it stores is Go's ninth floating argument.  main.saved
  // reads none: code the platform enters saves D8 for its caller, as
  // `runtime.sigtramp` does, and takes no argument in it.
  //   main.spill: str x9, [x0]; str d8, [x0, #8]; ret
  //   main.saved: str d8, [x0]; ret
  constexpr va_t Spill = kTextVA + 0x100;
  constexpr va_t Saved = kTextVA + 0x140;
  GoTestImage T(Arch::AArch64);
  T.writeText(Spill, words({0xF9000009, 0xFD000408, 0xD65F03C0}));
  T.writeText(Saved, words({0xFD000008, 0xD65F03C0}));
  for (const auto &[Address, Name] :
       {std::pair{Spill, "main.spill"}, std::pair{Saved, "main.saved"}}) {
    Symbol Function = Symbol::makeFunc(Address);
    Function.Name = Name;
    T.Img.Symbols.push_back(std::move(Function));
  }
  GoModuleInfo Module;
  Module.PclnTabMagic = 0xFFFFFFF1u;
  Module.RegisterArguments = true;
  Module.GoCode = {{kTextVA, kTextVA + 0x400}};
  Module.GoAssembly = {{Spill, Spill + 0x10}, {Saved, Saved + 0x10}};
  Module.FrameArguments = {{Spill, 0}, {Saved, 0}};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  llvm::LLVMContext Context;
  const PipelineResult R = Pipeline().run(T.Img, Context, Options);
  ASSERT_TRUE(R.Success) << R.Error;
  std::vector<uint64_t> SpillRegisters, SavedRegisters;
  for (const MedFunc &F : R.MedFuncs)
    for (const MedVar &Parameter : F.Params) {
      if (F.Entry == Spill)
        SpillRegisters.push_back(Parameter.RegOff);
      if (F.Entry == Saved)
        SavedRegisters.push_back(Parameter.RegOff);
    }
  EXPECT_EQ(SpillRegisters, (std::vector<uint64_t>{a64reg::X0, a64reg::V(8)}));
  EXPECT_EQ(SavedRegisters, std::vector<uint64_t>{a64reg::X0});
  const std::string Spilled = highCBody(R, T.Img, "main_spill");
  EXPECT_NE(Spilled.find("(double)(arg1)"), std::string::npos) << Spilled;
  const std::string Kept = highCBody(R, T.Img, "main_saved");
  EXPECT_NE(Kept.find("__builtin_trap"), std::string::npos) << Kept;
}

TEST(GoInternalABI, ARegisterNoParameterArrivesInIsNoPositionalParameter) {
  // A PE's platform passes its first argument in RCX.  Go's internal ABI
  // passes the third there, so a Go function of two arguments takes none in
  // it, and its entry RCX is no parameter.  Read by the platform's position
  // it printed as `arg0`, the argument RAX carries: main.f stored its first
  // argument through both, and main.g called through it.  Both formats now
  // print the same.
  //   main.f: mov [rax], rcx; mov [rbx], rcx; ret
  //   main.g: mov [rax], rbx; call rcx; ret
  constexpr va_t F = kTextVA + 0x100;
  constexpr va_t G = kTextVA + 0x140;
  std::string Bodies[2];
  for (const BinaryFormat Format : {BinaryFormat::ELF, BinaryFormat::COFF}) {
    SCOPED_TRACE(static_cast<unsigned>(Format));
    GoTestImage T(Arch::X64);
    T.Img.Format = Format;
    T.writeText(F, {0x48, 0x89, 0x08, 0x48, 0x89, 0x0B, 0xC3});
    T.writeText(G, {0x48, 0x89, 0x18, 0xFF, 0xD1, 0xC3});
    for (const auto &[Address, Name] :
         {std::pair{F, "main.f"}, std::pair{G, "main.g"}}) {
      Symbol Function = Symbol::makeFunc(Address);
      Function.Name = Name;
      T.Img.Symbols.push_back(std::move(Function));
    }
    GoModuleInfo Module;
    Module.PclnTabMagic = 0xFFFFFFF1u;
    Module.RegisterArguments = true;
    Module.GoCode = {{kTextVA, kTextVA + 0x400}};
    Module.FrameArguments = {{F, 16, 2}, {G, 16, 2}};
    T.Img.ExceptionMetadata.GoModule = std::move(Module);
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    llvm::LLVMContext Context;
    const PipelineResult R = Pipeline().run(T.Img, Context, Options);
    ASSERT_TRUE(R.Success) << R.Error;
    const std::string Stores = highCBody(R, T.Img, "main_f");
    EXPECT_NE(Stores.find("*(int64_t *)arg0 = "), std::string::npos) << Stores;
    EXPECT_NE(Stores.find("*(int64_t *)arg1 = "), std::string::npos) << Stores;
    // What each stores is a register no argument arrives in.
    EXPECT_EQ(Stores.find("= (uintptr_t)arg0;"), std::string::npos) << Stores;
    EXPECT_EQ(Stores.find("= arg0;"), std::string::npos) << Stores;
    EXPECT_NE(Stores.find("incoming register has no definition"),
              std::string::npos)
        << Stores;
    const std::string Calls = highCBody(R, T.Img, "main_g");
    EXPECT_NE(Calls.find("*(int64_t *)arg0 = arg1;"), std::string::npos)
        << Calls;
    EXPECT_EQ(Calls.find("(uintptr_t)((uintptr_t)arg0))("), std::string::npos)
        << Calls;
    EXPECT_NE(Calls.find("incoming register has no definition"),
              std::string::npos)
        << Calls;
    Bodies[Format == BinaryFormat::COFF] = Stores + Calls;
  }
  // The placeholders name the instruction, which both images place alike.
  EXPECT_EQ(Bodies[0], Bodies[1]);
}

TEST(GoStackABI, AnIndirectCallPassesNothingInARegister) {
  // Before the internal ABI a call into Go code passes every argument on the
  // stack.  main.m checks its goroutine's stack, so it runs there and calls
  // nothing but Go code: its call through a method table passes the word it
  // stored at its stack pointer and nothing else.  By the platform's
  // positions it passed whatever RDI, RSI and RDX held, and the goroutine
  // its stack check left in RCX.  main.n has no such check and calls through
  // a func value, the closure in RDX, after loading a word into RCX: no
  // argument either.  main.c, assembly without a check that calls through a
  // pointer it was given, may call C: it passes what it set in RDI.
  //   main.m: mov rcx, fs:[-8]; cmp rsp, [rcx+0x10]; jbe grow; sub rsp, 0x18
  //           mov rax, [rsp+0x20]; mov [rsp], rax; mov rax, [rsp+0x28]
  //           mov rax, [rax+0x18]; call rax; add rsp, 0x18; ret
  //     grow: call morestack; jmp main.m
  //   main.n: sub rsp, 0x10; mov rcx, [rsp+0x20]; mov rdx, [rsp+0x18]
  //           mov rax, [rdx]; call rax; add rsp, 0x10; ret
  //   main.c: sub rsp, 0x10; mov rax, [rsp+0x18]; mov rdi, [rsp+0x20]
  //           call rax; add rsp, 0x10; ret
  constexpr va_t M = kTextVA + 0x100;
  constexpr va_t N = kTextVA + 0x180;
  constexpr va_t C = kTextVA + 0x1C0;
  GoTestImage T(Arch::X64);
  std::vector<uint8_t> Code = {
      0x64, 0x48, 0x8B, 0x0C, 0x25, 0xF8, 0xFF, 0xFF, 0xFF, 0x48, 0x3B,
      0x61, 0x10, 0x76, 0x1D, 0x48, 0x83, 0xEC, 0x18, 0x48, 0x8B, 0x44,
      0x24, 0x20, 0x48, 0x89, 0x04, 0x24, 0x48, 0x8B, 0x44, 0x24, 0x28,
      0x48, 0x8B, 0x40, 0x18, 0xFF, 0xD0, 0x48, 0x83, 0xC4, 0x18, 0xC3};
  const std::vector<uint8_t> Grow = makeCall(M + Code.size(), Morestack);
  Code.insert(Code.end(), Grow.begin(), Grow.end());
  Code.insert(Code.end(), {0xEB, 0xCD});
  T.writeText(M, Code);
  T.writeText(Morestack, {0xC3});
  T.writeText(N, {0x48, 0x83, 0xEC, 0x10, 0x48, 0x8B, 0x4C, 0x24,
                  0x20, 0x48, 0x8B, 0x54, 0x24, 0x18, 0x48, 0x8B,
                  0x02, 0xFF, 0xD0, 0x48, 0x83, 0xC4, 0x10, 0xC3});
  T.writeText(C,
              {0x48, 0x83, 0xEC, 0x10, 0x48, 0x8B, 0x44, 0x24, 0x18, 0x48, 0x8B,
               0x7C, 0x24, 0x20, 0xFF, 0xD0, 0x48, 0x83, 0xC4, 0x10, 0xC3});
  for (const auto &[Address, Name] :
       {std::pair{M, "main.m"}, std::pair{N, "main.n"},
        std::pair{C, "main.c"}}) {
    Symbol Function = Symbol::makeFunc(Address);
    Function.Name = Name;
    T.Img.Symbols.push_back(std::move(Function));
  }
  GoModuleInfo Module;
  Module.StackArguments = true;
  Module.GoCode = {{kTextVA + 0x100, kTextVA + 0x500}};
  Module.MorestackEntries = {Morestack};
  Module.FrameArguments = {{M, 16}, {N, 16}, {C, 16}};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  llvm::LLVMContext Context;
  const PipelineResult R = Pipeline().run(T.Img, Context, Options);
  ASSERT_TRUE(R.Success) << R.Error;
  const std::string Method = highCBody(R, T.Img, "main_m");
  EXPECT_NE(Method.find(")(arg0);"), std::string::npos) << Method;
  EXPECT_EQ(Method.find("__builtin_trap"), std::string::npos) << Method;
  // The goroutine the check read reaches nothing now.
  EXPECT_EQ(Method.find("neverd_mem_load_fs"), std::string::npos) << Method;
  const std::string FuncValue = highCBody(R, T.Img, "main_n");
  EXPECT_NE(FuncValue.find(")))();"), std::string::npos) << FuncValue;
  EXPECT_EQ(FuncValue.find("__builtin_trap"), std::string::npos) << FuncValue;
  const std::string Pointer = highCBody(R, T.Img, "main_c");
  EXPECT_NE(Pointer.find(")(arg1);"), std::string::npos) << Pointer;
}

TEST(GoStackABI, ARegisterAStackRoutinePassesOnIsNoParameter) {
  // A stack-ABI routine takes nothing in a register.  One that sets RCX and
  // calls through a table passes on, by the platform's positions, whatever
  // RDI, RSI and RDX hold: call recovery names those entry values before it
  // knows the routine's parameters, and none is one.  Each printed as the
  // parameter at its platform position, a stack word the routine takes, or
  // past the last one as `arg-1`, which is no C.  Assembly of a register-ABI
  // image and code of a stack-ABI one print alike.
  //   main.d: mov rcx, [rsp+8]; mov rax, [rsp+16]; call [rax+rcx*8]; ret
  constexpr va_t D = kTextVA + 0x100;
  for (const bool RegisterABI : {true, false}) {
    SCOPED_TRACE(RegisterABI);
    GoTestImage T(Arch::X64);
    T.writeText(D, {0x48, 0x8B, 0x4C, 0x24, 0x08, 0x48, 0x8B, 0x44, 0x24, 0x10,
                    0xFF, 0x14, 0xC8, 0xC3});
    Symbol Function = Symbol::makeFunc(D);
    Function.Name = "main.d";
    T.Img.Symbols.push_back(std::move(Function));
    GoModuleInfo Module;
    Module.GoCode = {{kTextVA, kTextVA + 0x400}};
    if (RegisterABI) {
      Module.PclnTabMagic = 0xFFFFFFF1u;
      Module.RegisterArguments = true;
      Module.GoAssembly = {{D, D + 0x10}};
    } else {
      Module.StackArguments = true;
    }
    Module.FrameArguments = {{D, 16}};
    T.Img.ExceptionMetadata.GoModule = std::move(Module);
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    llvm::LLVMContext Context;
    const PipelineResult R = Pipeline().run(T.Img, Context, Options);
    ASSERT_TRUE(R.Success) << R.Error;
    const std::string Text = highCBody(R, T.Img, "main_d");
    EXPECT_EQ(Text.find("arg-1"), std::string::npos) << Text;
    // The word it loaded into RCX is the call's last argument ...
    EXPECT_NE(Text.find(", arg0);"), std::string::npos) << Text;
    // ... after three registers nothing gave a value, none of them the
    // routine's own stack words.
    size_t Unknown = 0;
    for (size_t At = Text.find("__builtin_trap"); At != std::string::npos;
         At = Text.find("__builtin_trap", At + 1))
      ++Unknown;
    EXPECT_EQ(Unknown, 3u) << Text;
    EXPECT_EQ(Text.find("(arg0, "), std::string::npos) << Text;
    EXPECT_EQ(Text.find(", arg1, "), std::string::npos) << Text;
  }
}

TEST(GoInternalABI, AFuncValueCallTakesItsArgumentsInGoRegisters) {
  // A call through a func value enters Go code by the internal ABI, which
  // passes integer arguments in RAX, RBX, RCX, RDI, RSI and R8-R11.  The
  // call took the platform's RDI and RSI instead, and dead code elimination
  // dropped the writes of RAX and RBX, which System V passes nothing in.  Go
  // has no shadow space and no positions.  Read as Win64's, the local main.g
  // stores at [rsp+0x24] was a stack argument, and it made every register an
  // argument that reaches the call: RDI, set before a branch, and RSI.
  //   main.g: push rbp; mov rbp, rsp; sub rsp, 0x30; mov [rsp+0x28], rax
  //           mov edi, 9; test rax, rax; jne 2f; xor esi, esi; jmp 1f
  //        2: xor r12d, r12d
  //        1: mov dword [rsp+0x24], 3; mov rdx, [rsp+0x28]; mov eax, 5
  //           mov ebx, 7; call [rdx]; add rsp, 0x30; pop rbp; ret
  constexpr va_t G = kTextVA + 0x100;
  for (const BinaryFormat Format : {BinaryFormat::ELF, BinaryFormat::COFF}) {
    SCOPED_TRACE(static_cast<unsigned>(Format));
    GoTestImage T(Arch::X64);
    T.Img.Format = Format;
    T.writeText(G, {0x55, 0x48, 0x89, 0xE5, 0x48, 0x83, 0xEC, 0x30, 0x48,
                    0x89, 0x44, 0x24, 0x28, 0xBF, 0x09, 0x00, 0x00, 0x00,
                    0x48, 0x85, 0xC0, 0x75, 0x04, 0x31, 0xF6, 0xEB, 0x03,
                    0x45, 0x31, 0xE4, 0xC7, 0x44, 0x24, 0x24, 0x03, 0x00,
                    0x00, 0x00, 0x48, 0x8B, 0x54, 0x24, 0x28, 0xB8, 0x05,
                    0x00, 0x00, 0x00, 0xBB, 0x07, 0x00, 0x00, 0x00, 0xFF,
                    0x12, 0x48, 0x83, 0xC4, 0x30, 0x5D, 0xC3});
    Symbol Function = Symbol::makeFunc(G);
    Function.Name = "main.g";
    T.Img.Symbols.push_back(std::move(Function));
    GoModuleInfo Module;
    Module.PclnTabMagic = 0xFFFFFFF1u;
    Module.RegisterArguments = true;
    Module.GoCode = {{kTextVA, kTextVA + 0x400}};
    T.Img.ExceptionMetadata.GoModule = std::move(Module);
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    llvm::LLVMContext Context;
    const PipelineResult R = Pipeline().run(T.Img, Context, Options);
    ASSERT_TRUE(R.Success) << R.Error;
    EXPECT_EQ(R.MedIRVerifierFailures, 0u);
    const std::string Text = highCBody(R, T.Img, "main_g");
    ASSERT_FALSE(Text.empty());
    EXPECT_NE(Text.find("((uint64_t)5, (uint64_t)7);"), std::string::npos)
        << Text;
    EXPECT_EQ(Text.find("__builtin_trap"), std::string::npos) << Text;
  }
}

TEST(GoInternalABI, TheRegisterThatHoldsTheTargetEndsTheArguments) {
  // Go assigns its integer arguments to RAX, RBX, RCX and on in order, so a
  // func value called through RCX takes two.  Go 1.21's map access loads the
  // seed into ESI before RBX takes it; RSI, Go's fifth register, made the
  // call take five, the target and an RDI nobody set among them.
  //   main.g: push rbp; mov rbp, rsp; sub rsp, 0x20; mov rdx, [rax+0x48]
  //           mov rcx, [rdx]; mov esi, [rbx+0xc]; lea rax, [rsp+0x18]
  //           mov rbx, rsi; call rcx; add rsp, 0x20; pop rbp; ret
  constexpr va_t G = kTextVA + 0x100;
  GoTestImage T(Arch::X64);
  T.writeText(G, {0x55, 0x48, 0x89, 0xE5, 0x48, 0x83, 0xEC, 0x20, 0x48,
                  0x8B, 0x50, 0x48, 0x48, 0x8B, 0x0A, 0x8B, 0x73, 0x0C,
                  0x48, 0x8D, 0x44, 0x24, 0x18, 0x48, 0x89, 0xF3, 0xFF,
                  0xD1, 0x48, 0x83, 0xC4, 0x20, 0x5D, 0xC3});
  Symbol Function = Symbol::makeFunc(G);
  Function.Name = "main.g";
  T.Img.Symbols.push_back(std::move(Function));
  GoModuleInfo Module;
  Module.PclnTabMagic = 0xFFFFFFF1u;
  Module.RegisterArguments = true;
  Module.GoCode = {{kTextVA, kTextVA + 0x400}};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  llvm::LLVMContext Context;
  const PipelineResult R = Pipeline().run(T.Img, Context, Options);
  ASSERT_TRUE(R.Success) << R.Error;
  EXPECT_EQ(R.MedIRVerifierFailures, 0u);
  const std::string Text = highCBody(R, T.Img, "main_g");
  ASSERT_FALSE(Text.empty());
  EXPECT_NE(Text.find("(uintptr_t)&stack_storage.var_m10, "
                      "(int64_t)(*(uint32_t *)(arg1 + 0xC)));"),
            std::string::npos)
      << Text;
  EXPECT_EQ(Text.find("__builtin_trap"), std::string::npos) << Text;
}

TEST(GoInternalABI, AJoinOfWhatACallLeftIsNoFuncValueArgument) {
  // A compiler sets each argument of a call on every way to it.  Where one
  // way to a func value call sets X0 and the other calls main.f, X0 at the
  // call joins zero with what main.f left: it is no argument
  // (setOnEveryWay).  Taken as one, the call passed an unknown value after
  // RAX.
  //   main.f: ret
  //   main.g: push rbp; mov rbp, rsp; sub rsp, 0x20; mov [rsp+0x10], rax
  //           test rax, rax; jne 2f; xorps xmm0, xmm0; jmp 1f
  //        2: call main.f
  //        1: mov rdx, [rsp+0x10]; mov eax, 5; call [rdx]
  //           add rsp, 0x20; pop rbp; ret
  constexpr va_t F = kTextVA + 0x80;
  constexpr va_t G = kTextVA + 0x100;
  GoTestImage T(Arch::X64);
  T.writeText(F, {0xC3});
  std::vector<uint8_t> Body = {0x55, 0x48, 0x89, 0xE5, 0x48, 0x83, 0xEC, 0x20,
                               0x48, 0x89, 0x44, 0x24, 0x10, 0x48, 0x85, 0xC0,
                               0x75, 0x05, 0x0F, 0x57, 0xC0, 0xEB, 0x05};
  const std::vector<uint8_t> Call = makeCall(G + Body.size(), F);
  Body.insert(Body.end(), Call.begin(), Call.end());
  Body.insert(Body.end(),
              {0x48, 0x8B, 0x54, 0x24, 0x10, 0xB8, 0x05, 0x00, 0x00, 0x00, 0xFF,
               0x12, 0x48, 0x83, 0xC4, 0x20, 0x5D, 0xC3});
  T.writeText(G, Body);
  for (const auto &[Entry, Name] :
       {std::pair{F, "main.f"}, std::pair{G, "main.g"}}) {
    Symbol Function = Symbol::makeFunc(Entry);
    Function.Name = Name;
    T.Img.Symbols.push_back(std::move(Function));
  }
  GoModuleInfo Module;
  Module.PclnTabMagic = 0xFFFFFFF1u;
  Module.RegisterArguments = true;
  Module.GoCode = {{kTextVA, kTextVA + 0x400}};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  llvm::LLVMContext Context;
  const PipelineResult R = Pipeline().run(T.Img, Context, Options);
  ASSERT_TRUE(R.Success) << R.Error;
  EXPECT_EQ(R.MedIRVerifierFailures, 0u);
  const std::string Text = highCBody(R, T.Img, "main_g");
  ASSERT_FALSE(Text.empty());
  EXPECT_NE(Text.find("((uint64_t)5);"), std::string::npos) << Text;
  EXPECT_EQ(Text.find("__builtin_trap"), std::string::npos) << Text;
}

TEST(GoFunctionTable, ABranchInsideARecordedFunctionStaysInIt) {
  // A branch into a recorded function's interior is no tail call, whatever
  // names the label: `runtime.runGCProg`'s jump to its loop became a call to
  // a function nothing defined, and the loop was decompiled nowhere.
  //   main.f: jmp main.f+8; nop x6; mov eax, 1; ret
  //   main.g: call main.f+8; ret
  constexpr va_t F = kTextVA + 0x100;
  constexpr va_t G = kTextVA + 0x200;
  GoTestImage T;
  T.writeText(F, {0xEB, 0x06, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0xB8, 0x01,
                  0x00, 0x00, 0x00, 0xC3});
  std::vector<uint8_t> Caller = makeCall(G, F + 8);
  Caller.push_back(0xC3);
  T.writeText(G, Caller);
  for (const auto &[Entry, Name] :
       {std::pair{F, "main.f"}, std::pair{G, "main.g"}}) {
    Symbol Function = Symbol::makeFunc(Entry);
    Function.Name = Name;
    T.Img.Symbols.push_back(std::move(Function));
  }
  GoModuleInfo Module;
  Module.PclnTabMagic = 0xFFFFFFF1u;
  Module.RegisterArguments = true;
  Module.GoCode = {{F, kTextVA + 0x300}};
  Module.FrameArguments = {{F, 0, 0}, {G, 0, 0}};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  llvm::LLVMContext Context;
  const PipelineResult R = Pipeline().run(T.Img, Context, Options);
  ASSERT_TRUE(R.Success) << R.Error;
  const auto It =
      llvm::find_if(R.MedFuncs, [&](const MedFunc &M) { return M.Entry == F; });
  ASSERT_NE(It, R.MedFuncs.end());
  EXPECT_FALSE(hasOp(*It, NdOp::CALL));
}

TEST(GoInternalABI, AResultIsTheArgumentWhereAReturnLeavesItUnchanged) {
  // A Go result is the whole register a return leaves: the argument the
  // caller passed in it where the path leaves it unchanged, though another
  // path writes it (`if n.Bytes == nil { return "" }` returns the nil
  // receiver as the pointer), past a join too, and not the entry copy of
  // its low byte that a read of AL leaves.
  //   main.f: test [rax], al; jne 1f; xor ebx, ebx; ret
  //       1:  test rbx, rbx; je 2f; cmp byte [rax], 1; je 2f
  //           mov rax, [rax+8]; ret
  //       2:  xor ebx, ebx; ret
  constexpr va_t F = kTextVA + 0x100;
  constexpr va_t G = kTextVA + 0x180;
  GoTestImage T = goFunctions(
      {{{F, "main.f"}, {0x84, 0x00, 0x75, 0x03, 0x31, 0xDB, 0xC3, 0x48, 0x85,
                        0xDB, 0x74, 0x0A, 0x80, 0x38, 0x01, 0x74, 0x05, 0x48,
                        0x8B, 0x40, 0x08, 0xC3, 0x31, 0xDB, 0xC3}},
       {{G, "main.g"}, readsRBXAfter(G, F)}});
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  llvm::LLVMContext Context;
  const PipelineResult R = Pipeline().run(T.Img, Context, Options);
  ASSERT_TRUE(R.Success) << R.Error;
  const auto It =
      llvm::find_if(R.MedFuncs, [&](const MedFunc &M) { return M.Entry == F; });
  ASSERT_NE(It, R.MedFuncs.end());
  ASSERT_EQ(It->MultiReturn.size(), 2u);
  EXPECT_EQ(It->MultiReturn[1].RegOff, x86reg::RBX);
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions C;
  C.TheArch = T.Img.Arch;
  C.Format = T.Img.Format;
  C.Image = &T.Img;
  ASSERT_TRUE(HighCEmitter().emit(R.HighFuncs, OS, C));
  OS.flush();
  // The definition, not its declaration or main.g's call.
  size_t At = 0;
  while ((At = Source.find(" main_f(", At)) != std::string::npos &&
         Source.compare(Source.find('\n', At) - 2, 2, " {") != 0)
    ++At;
  ASSERT_NE(At, std::string::npos) << Source;
  const std::string Body = Source.substr(At, Source.find("\n}", At) - At);
  EXPECT_EQ(Body.find("__builtin_trap"), std::string::npos) << Body;
}

TEST(GoRegisterArguments, NoMoreRegistersThanItsArgumentsHaveScalars) {
  // A traceback lists each argument's scalar components (FUNCDATA_ArgInfo),
  // and each takes one register at most: `sync/atomic.StorePointer(addr,
  // val)` lists two.  Its body reads more, which AAPCS64 positions took as
  // parameters, and every call passed unknowns for them.
  //   main.f: str x2, [x0]; str x3, [x1]; ret
  //   main.g: stp x29, x30, [sp, #-16]!; mov x29, sp; mov x2, #1
  //           mov x3, #2; bl main.f; ldp x29, x30, [sp], #16; ret
  constexpr va_t F = kTextVA + 0x100;
  constexpr va_t G = kTextVA + 0x180;
  GoTestImage T(Arch::AArch64);
  T.writeText(F, words({0xF9000002, 0xF9000023, 0xD65F03C0}));
  const uint32_t Call = 0x94000000u | (((F - (G + 16)) >> 2) & 0x3FFFFFFu);
  T.writeText(G, words({0xA9BF7BFD, 0x910003FD, 0xD2800022, 0xD2800043, Call,
                        0xA8C17BFD, 0xD65F03C0}));
  for (const auto &[Entry, Name] :
       {std::pair{F, "main.f"}, std::pair{G, "main.g"}}) {
    Symbol Function = Symbol::makeFunc(Entry);
    Function.Name = Name;
    T.Img.Symbols.push_back(std::move(Function));
  }
  GoModuleInfo Module;
  Module.PclnTabMagic = 0xFFFFFFF1u;
  Module.RegisterArguments = true;
  Module.GoCode = {{kTextVA, kTextVA + 0x400}};
  Module.FrameArguments = {{F, 16, 2}, {G, 0, 0}};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  llvm::LLVMContext Context;
  const PipelineResult R = Pipeline().run(T.Img, Context, Options);
  ASSERT_TRUE(R.Success) << R.Error;
  const auto Function = [&](va_t Entry) -> const MedFunc & {
    const auto It = llvm::find_if(
        R.MedFuncs, [&](const MedFunc &M) { return M.Entry == Entry; });
    EXPECT_NE(It, R.MedFuncs.end());
    return *It;
  };
  const MedFunc &Callee = Function(F);
  EXPECT_EQ(Callee.MaxRegisterArguments, 2);
  ASSERT_EQ(Callee.Params.size(), 2u) << Callee.Params.size();
  EXPECT_EQ(Callee.Params[0].RegOff, a64reg::X0);
  EXPECT_EQ(Callee.Params[1].RegOff, a64reg::X1);
  for (const MedCallInfo &Info : Function(G).CallInfos)
    if (Info.TargetAddr == F)
      EXPECT_LE(Info.Args.size(), 2u);
}

TEST(GoRegisterArguments, TheSpillAreaHoldsNoStackArgument) {
  // Where every argument is a register one, the area above the return
  // address is all spill slots, which the function fills before it reads
  // them across a call: `runtime.badsignal(sig, c)`.  Read as stack
  // arguments, they put AAPCS64's eight register positions before them.
  //   main.f: str x30, [sp, #-32]!; str x0, [sp, #40]; str x1, [sp, #48]
  //           bl main.g; ldr x0, [sp, #40]; ldr x1, [sp, #48]
  //           add x0, x0, x1; ldr x30, [sp], #32; ret
  //   main.g: ret
  constexpr va_t F = kTextVA + 0x100;
  constexpr va_t G = kTextVA + 0x180;
  GoTestImage T(Arch::AArch64);
  const uint32_t Call = 0x94000000u | (((G - (F + 12)) >> 2) & 0x3FFFFFFu);
  T.writeText(F, words({0xF81E0FFE, 0xF90017E0, 0xF9001BE1, Call, 0xF94017E0,
                        0xF9401BE1, 0x8B010000, 0xF84207FE, 0xD65F03C0}));
  T.writeText(G, words({0xD65F03C0}));
  for (const auto &[Entry, Name] :
       {std::pair{F, "main.f"}, std::pair{G, "main.g"}}) {
    Symbol Function = Symbol::makeFunc(Entry);
    Function.Name = Name;
    T.Img.Symbols.push_back(std::move(Function));
  }
  GoModuleInfo Module;
  Module.PclnTabMagic = 0xFFFFFFF1u;
  Module.RegisterArguments = true;
  Module.GoCode = {{kTextVA, kTextVA + 0x400}};
  Module.FrameArguments = {{F, 16, 2}, {G, 0, 0}};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  llvm::LLVMContext Context;
  const PipelineResult R = Pipeline().run(T.Img, Context, Options);
  ASSERT_TRUE(R.Success) << R.Error;
  const auto It =
      llvm::find_if(R.MedFuncs, [&](const MedFunc &M) { return M.Entry == F; });
  ASSERT_NE(It, R.MedFuncs.end());
  ASSERT_EQ(It->Params.size(), 2u) << It->Params.size();
  EXPECT_EQ(It->Params[0].RegOff, a64reg::X0);
  EXPECT_EQ(It->Params[1].RegOff, a64reg::X1);
}

TEST(GoInternalABI, AnARM64DirectCallReturnsEveryResultItsCallerReads) {
  // Go's arm64 register ABI returns results in R0-R15 as amd64 does in RAX,
  // RBX and the rest: a read of X2 after a call is its third result, in the
  // call and in the function it calls, where AAPCS64 returns X0:X1 at most.
  //   main.f: ldr x1, [x0, #8]; ldr x2, [x0, #16]; ldr x0, [x0]; ret
  //   main.g: stp x29, x30, [sp, #-16]!; mov x29, sp; bl main.f
  //           mov x0, x2; ldp x29, x30, [sp], #16; ret
  constexpr va_t F = kTextVA + 0x100;
  constexpr va_t G = kTextVA + 0x180;
  GoTestImage T(Arch::AArch64);
  T.writeText(F, words({0xF9400401, 0xF9400802, 0xF9400000, 0xD65F03C0}));
  const uint32_t Call = 0x94000000u | (((F - (G + 8)) >> 2) & 0x3FFFFFFu);
  T.writeText(G, words({0xA9BF7BFD, 0x910003FD, Call, 0xAA0203E0, 0xA8C17BFD,
                        0xD65F03C0}));
  for (const auto &[Entry, Name] :
       {std::pair{F, "main.f"}, std::pair{G, "main.g"}}) {
    Symbol Function = Symbol::makeFunc(Entry);
    Function.Name = Name;
    T.Img.Symbols.push_back(std::move(Function));
  }
  GoModuleInfo Module;
  Module.PclnTabMagic = 0xFFFFFFF1u;
  Module.RegisterArguments = true;
  Module.GoCode = {{kTextVA, kTextVA + 0x400}};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  llvm::LLVMContext Context;
  const PipelineResult R = Pipeline().run(T.Img, Context, Options);
  ASSERT_TRUE(R.Success) << R.Error;
  const auto Function = [&](va_t Entry) -> const MedFunc & {
    const auto It = llvm::find_if(
        R.MedFuncs, [&](const MedFunc &M) { return M.Entry == Entry; });
    EXPECT_NE(It, R.MedFuncs.end());
    return *It;
  };
  const auto ReturnsX0ToX2 = [](const std::vector<MedReturnReg> &Shape) {
    return Shape.size() == 3 && Shape[0].RegOff == a64reg::X0 &&
           Shape[1].RegOff == a64reg::X1 && Shape[2].RegOff == a64reg::X2 &&
           Shape[2].Size == 8;
  };
  EXPECT_TRUE(ReturnsX0ToX2(Function(F).MultiReturn));
  const MedOp *Read = onlyCall(Function(G));
  ASSERT_NE(Read, nullptr);
  ASSERT_TRUE(Read->RegisterReturnFields);
  EXPECT_TRUE(ReturnsX0ToX2(*Read->RegisterReturnFields));
}

TEST(GoInternalABI, TwoARM64ResultsAreARecord) {
  // Go returns a list of results, which C carries as a record: two arm64
  // results in X0 and X1 are no double-word integer, as AAPCS64's pair is.
  // As one, `runtime.makeAddrRange`, which returns its two arguments, was
  // refused, its high word being no value the function computed.
  //   main.f: stp x29, x30, [sp, #-16]!; mov x29, sp; cbz x0, 1f
  //           cmp x0, x1; b.hi 1f; ldp x29, x30, [sp], #16; ret
  //       1:  mov x1, #60; bl main.h; brk #1
  //   main.h: ret
  //   main.g: stp x29, x30, [sp, #-16]!; mov x29, sp; mov x0, #1
  //           mov x1, #2; bl main.f; add x0, x0, x1
  //           ldp x29, x30, [sp], #16; ret
  constexpr va_t F = kTextVA + 0x100;
  constexpr va_t H = kTextVA + 0x140;
  constexpr va_t G = kTextVA + 0x180;
  GoTestImage T(Arch::AArch64);
  const uint32_t Trap = 0x94000000u | (((H - (F + 32)) >> 2) & 0x3FFFFFFu);
  T.writeText(F,
              words({0xA9BF7BFD, 0x910003FD, 0xB40000A0, 0xEB01001F, 0x54000068,
                     0xA8C17BFD, 0xD65F03C0, 0xD2800781, Trap, 0xD4200020}));
  T.writeText(H, words({0xD65F03C0}));
  const uint32_t Call = 0x94000000u | (((F - (G + 16)) >> 2) & 0x3FFFFFFu);
  T.writeText(G, words({0xA9BF7BFD, 0x910003FD, 0xD2800020, 0xD2800041, Call,
                        0x8B010000, 0xA8C17BFD, 0xD65F03C0}));
  for (const auto &[Entry, Name] :
       {std::pair{F, "main.f"}, std::pair{H, "main.h"},
        std::pair{G, "main.g"}}) {
    Symbol Function = Symbol::makeFunc(Entry);
    Function.Name = Name;
    T.Img.Symbols.push_back(std::move(Function));
  }
  GoModuleInfo Module;
  Module.PclnTabMagic = 0xFFFFFFF1u;
  Module.RegisterArguments = true;
  Module.GoCode = {{kTextVA, kTextVA + 0x400}};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  llvm::LLVMContext Context;
  const PipelineResult R = Pipeline().run(T.Img, Context, Options);
  ASSERT_TRUE(R.Success) << R.Error;
  const auto It =
      llvm::find_if(R.MedFuncs, [&](const MedFunc &M) { return M.Entry == F; });
  ASSERT_NE(It, R.MedFuncs.end());
  ASSERT_EQ(It->MultiReturn.size(), 2u);
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions C;
  C.TheArch = T.Img.Arch;
  C.Format = T.Img.Format;
  C.Image = &T.Img;
  ASSERT_TRUE(HighCEmitter().emit(R.HighFuncs, OS, C));
  OS.flush();
  // The definition, not its declaration or main.g's call.
  size_t At = 0;
  while ((At = Source.find(" main_f(", At)) != std::string::npos &&
         Source.compare(Source.find('\n', At) - 2, 2, " {") != 0)
    ++At;
  ASSERT_NE(At, std::string::npos) << Source;
  const size_t Line = Source.rfind('\n', At) + 1;
  const std::string Signature = Source.substr(Line, At - Line);
  EXPECT_EQ(Signature.find("__int128"), std::string::npos) << Signature;
  const std::string Body = Source.substr(At, Source.find("\n}", At) - At);
  EXPECT_EQ(Body.find("no recovered semantics"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("has no definition"), std::string::npos) << Body;
}

TEST(GoInternalABI, AStaleViewReadsNoResultOfTheCall) {
  // arm64's W11 is a value of its own beside X11, which `mov x11, #45`
  // writes whole and leaves W11's old version behind.  Where `ubfx x12,
  // x11, #0, #8` reads the low byte after the paths join, the W11 PHI's
  // stale arm is what the call left, which `internal/bisect.New` read as
  // `new`'s twelfth result, raising every allocation that returns its
  // call's results.
  //   main.g: stp x29, x30, [sp, #-16]!; mov x29, sp; bl main.f
  //           cbz x0, 1f; ldrb w11, [x0]; b 2f
  //       1:  mov x11, #45
  //       2:  ubfx x12, x11, #0, #8; mov x0, x12
  //           ldp x29, x30, [sp], #16; ret
  constexpr va_t F = kTextVA + 0x100;
  constexpr va_t G = kTextVA + 0x180;
  GoTestImage T(Arch::AArch64);
  T.writeText(F, words({0xF9400000, 0xD65F03C0}));
  const uint32_t Call = 0x94000000u | (((F - (G + 8)) >> 2) & 0x3FFFFFFu);
  T.writeText(G, words({0xA9BF7BFD, 0x910003FD, Call, 0xB4000060, 0x3940000B,
                        0x14000002, 0xD28005AB, 0xD3401D6C, 0xAA0C03E0,
                        0xA8C17BFD, 0xD65F03C0}));
  GoModuleInfo Module;
  Module.PclnTabMagic = 0xFFFFFFF1u;
  Module.RegisterArguments = true;
  Module.GoCode = {{kTextVA, kTextVA + 0x400}};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  const MedFunc Caller = lift(T.Img, G);
  const MedOp *Read = onlyCall(Caller);
  ASSERT_NE(Read, nullptr);
  EXPECT_TRUE(
      goCallResultsRead(Caller, Arch::AArch64, {Read->CallSiteId}).empty());
}

TEST(GoInternalABI, OrdinaryCallsKeepX15ButMayChangeTheGoroutine) {
  // Runtime calls can change g in R14; X15 is still zero across ordinary
  // compiled Go calls. Assembly callers do not get the same X15 contract.
  const BinaryImage Img = registerArgumentImage();
  const MedFunc G = lift(Img, GoCaller);
  const MedOp *Call = onlyCall(G);
  ASSERT_NE(Call, nullptr);
  EXPECT_EQ(Call->RegisterContract, CallRegisterContract::GoInternal);
  const TargetRegInfo &TRI = getTargetRegInfo(Img.Arch);
  EXPECT_TRUE(callClobbersRegister(*Call, x86reg::R14, 8, TRI, Img.Format));
  EXPECT_FALSE(callClobbersRegister(*Call, x86reg::XMM15, 16, TRI, Img.Format));
  EXPECT_TRUE(callClobbersRegister(*Call, x86reg::RBX, 8, TRI, Img.Format));
  EXPECT_EQ(callRegisterContractOf(Img, Assembly, GoCallee),
            CallRegisterContract::GoCode);
}

TEST(GoInternalABI, ABufferWriteBarrierKeepsR14AndX15ButNotItsResult) {
  const TargetRegInfo &TRI = getTargetRegInfo(Arch::X64);
  const auto Contract = CallRegisterContract::GoInternalWriteBarrier;
  const auto Result =
      goWriteBarrierResult(Arch::X64, GoWriteBarrierKind::Buffer);
  ASSERT_EQ(Result, x86reg::R11);
  EXPECT_EQ(goContractKeptBytes(TRI, Contract, x86reg::R14, 8, Result), 8u);
  EXPECT_EQ(goContractKeptBytes(TRI, Contract, x86reg::XMM15, 16, Result), 16u);
  EXPECT_EQ(goContractKeptBytes(TRI, Contract, x86reg::R11, 8, Result), 0u);
}

TEST(GoInternalABI, X15ReadsAsZero) {
  // main.z: movups [rcx], xmm15; ret: Go's internal ABI enters it with X15
  // zero, so it stores sixteen zero bytes.
  BinaryImage Img = registerArgumentImage();
  constexpr va_t Zeroing = kTextVA + 0x380;
  GoTestImage T;
  T.Img = std::move(Img);
  T.writeText(Zeroing, {0x44, 0x0F, 0x11, 0x39, 0xC3});
  const MedFunc F = lift(T.Img, Zeroing);
  const MedOp *Store = nullptr;
  for (const MedBlock &Block : F.Blocks)
    for (const MedOp &Op : Block.Ops)
      if (Op.Opcode == NdOp::STORE)
        Store = &Op;
  ASSERT_NE(Store, nullptr);
  ASSERT_GE(Store->NumInputs, 2u);
  const MedVar &Value = Store->Inputs[1];
  bool Zero = Value.Kind == MedVar::Const && Value.ConstVal == 0;
  for (const MedBlock &Block : F.Blocks)
    for (const MedOp &Op : Block.Ops)
      if (Op.Opcode == NdOp::COPY && Op.Output.Kind == Value.Kind &&
          Op.Output.Id == Value.Id && Op.Output.SSAVer == Value.SSAVer &&
          Op.NumInputs == 1 && Op.Inputs[0].Kind == MedVar::Const &&
          Op.Inputs[0].ConstVal == 0)
        Zero = true;
  EXPECT_TRUE(Zero);
}

TEST(GoPrologueSaves, AStoreOfRBXIsNoPrologueSave) {
  // main.f: push rbp; mov rbp, rsp; sub rsp, 0x20; mov [rsp+0x18], rbx;
  // add rsp, 0x20; pop rbp; ret.  C keeps RBX for its caller, so this is a
  // save of it.  Go keeps none but RBP: RBX is its second integer argument,
  // and runtime.throw stores it so into the closure it hands systemstack.
  BinaryImage Img = registerArgumentImage();
  constexpr va_t Function = kTextVA + 0x380;
  GoTestImage T;
  T.Img = std::move(Img);
  T.writeText(Function,
              {0x55, 0x48, 0x89, 0xE5, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x89, 0x5C,
               0x24, 0x18, 0x48, 0x83, 0xC4, 0x20, 0x5D, 0xC3});
  ASSERT_TRUE(isGoCode(T.Img, Function));
  const auto Saves = [&](const BinaryImage *Image) {
    MedFunc F = lift(T.Img, Function);
    markSourceStackAccesses(F, T.Img.Arch, T.Img.Format, StackEntryKind::Call,
                            Image);
    std::map<uint64_t, bool> SavedRegisters;
    for (const MedBlock &Block : F.Blocks)
      for (const MedOp &Op : Block.Ops)
        if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2 &&
            Op.Inputs[1].Kind == MedVar::Reg)
          SavedRegisters[Op.Inputs[1].RegOff] = Op.PrologueSave;
    return SavedRegisters;
  };
  const std::map<uint64_t, bool> AsC = Saves(nullptr);
  ASSERT_TRUE(AsC.count(x86reg::RBX));
  EXPECT_TRUE(AsC.at(x86reg::RBX));
  const std::map<uint64_t, bool> AsGo = Saves(&T.Img);
  ASSERT_TRUE(AsGo.count(x86reg::RBX));
  EXPECT_FALSE(AsGo.at(x86reg::RBX));
  // Go keeps its frame pointer as C does.
  ASSERT_TRUE(AsGo.count(x86reg::RBP));
  EXPECT_TRUE(AsGo.at(x86reg::RBP));
}

TEST(GoClosureCalls, TheContextRegisterIsNoArgument) {
  // runtime.systemstack: mov rdi, [rsp+8]; mov rdx, rdi; mov rdi, [rdi];
  // call rdi; ret.  Go calls the func value with its closure in RDX.  Read
  // as System V's, setting RDX made RDI and an unset RSI arguments before
  // it, and the routine took the caller's RSI as a parameter.
  BinaryImage Img = registerArgumentImage();
  constexpr va_t GoAssembler = kTextVA + 0x480;
  constexpr va_t CCode = kTextVA + 0x600;
  GoTestImage T;
  T.Img = std::move(Img);
  const std::vector<uint8_t> Body = {0x48, 0x8B, 0x7C, 0x24, 0x08, 0x48, 0x89,
                                     0xFA, 0x48, 0x8B, 0x3F, 0xFF, 0xD7, 0xC3};
  T.writeText(GoAssembler, Body);
  T.writeText(CCode, Body);
  ASSERT_TRUE(isGoCode(T.Img, GoAssembler));
  ASSERT_FALSE(usesGoRegisterArguments(T.Img, GoAssembler));
  ASSERT_FALSE(isGoCode(T.Img, CCode));
  const auto Arguments = [&](va_t Entry) {
    MedFunc F = lift(T.Img, Entry);
    recoverCallAbi(F, T.Img.Arch, {}, &T.Img);
    EXPECT_EQ(F.CallInfos.size(), 1u);
    return F.CallInfos.empty() ? std::vector<MedVar>{} : F.CallInfos[0].Args;
  };
  EXPECT_TRUE(Arguments(GoAssembler).empty());
  // C code passes what it sets up: RDI, RSI and RDX.
  EXPECT_EQ(Arguments(CCode).size(), 3u);
}

TEST(GoCallbacks, AnEntryTailCallsTheRoutineThatReturnsPastIt) {
  // runtime.callbackasm: call callbackasm1; call callbackasm1; call
  // callbackasm1.  Each call leaves its entry's address, the callback's
  // index, for callbackasm1, which returns to the entry's caller.  Read as
  // returning calls, the 2,000 entries were one function of 2,000 calls,
  // each after the first passing four unknown arguments.
  BinaryImage Img = registerArgumentImage();
  constexpr va_t Table = kTextVA + 0x600;
  constexpr va_t Callback = kTextVA + 0x700;
  GoTestImage T;
  T.Img = std::move(Img);
  std::vector<uint8_t> Entries;
  for (va_t At = Table; At < Table + 15; At += 5) {
    const std::vector<uint8_t> Call = makeCall(At, Callback);
    Entries.insert(Entries.end(), Call.begin(), Call.end());
  }
  // A return ends the table, so that the entries read as returning calls
  // stop there rather than in what follows.
  Entries.push_back(0xC3);
  T.writeText(Table, Entries);
  T.writeText(Callback, {0xC3});
  const auto Calls = [&] {
    unsigned Count = 0;
    bool Returns = false;
    for (const LowBlock &Block : build(T.Img, Table).Blocks)
      for (const LowOp &Op : Block.Ops) {
        Count += Op.Opcode == NdOp::CALL;
        Returns |= Op.Opcode == NdOp::RETURN;
      }
    return std::pair{Count, Returns};
  };
  EXPECT_EQ(Calls(), std::pair(3u, true));
  T.Img.ExceptionMetadata.GoModule->ReturnsPastCallerEntries = {Callback};
  ASSERT_TRUE(goCallReturnsPastCaller(T.Img, Callback));
  EXPECT_EQ(Calls(), std::pair(1u, true));
}

TEST(GoStackArguments, ArgumentsAndTheirCallsAreOnTheStack) {
  // Go 1.16: main.add(a, b int) int reads its arguments past the return
  // address and stores its result after them:
  //   mov rax, [rsp+8]; add rax, [rsp+16]; mov [rsp+24], rax; ret
  // and main.caller passes them from its stack pointer:
  //   sub rsp, 0x20; mov qword [rsp], 1; mov qword [rsp+8], 2;
  //   call main.add; mov rax, [rsp+16]; add rsp, 0x20; ret
  // Read as System V's, stack arguments followed six register positions,
  // as in runtime_throw(0, 0, 0, 0, 0, 0, msg, len).
  constexpr va_t Add = kTextVA + 0x100;
  constexpr va_t Caller = kTextVA + 0x200;
  GoTestImage T;
  T.writeText(Add, {0x48, 0x8B, 0x44, 0x24, 0x08, 0x48, 0x03, 0x44, 0x24, 0x10,
                    0x48, 0x89, 0x44, 0x24, 0x18, 0xC3});
  std::vector<uint8_t> Body = {0x48, 0x83, 0xEC, 0x20, 0x48, 0xC7, 0x04,
                               0x24, 0x01, 0x00, 0x00, 0x00, 0x48, 0xC7,
                               0x44, 0x24, 0x08, 0x02, 0x00, 0x00, 0x00};
  const std::vector<uint8_t> Call = makeCall(Caller + Body.size(), Add);
  Body.insert(Body.end(), Call.begin(), Call.end());
  Body.insert(Body.end(),
              {0x48, 0x8B, 0x44, 0x24, 0x10, 0x48, 0x83, 0xC4, 0x20, 0xC3});
  T.writeText(Caller, Body);
  GoModuleInfo Module;
  Module.StackArguments = true;
  Module.GoCode = {{Add, kTextVA + 0x300}};
  Module.FrameArguments = {{Add, 24}, {Caller, 0}};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  ASSERT_TRUE(goTakesStackArguments(T.Img, Add));

  const MedFunc F = lift(T.Img, Add);
  EXPECT_TRUE(F.StackArgumentsOnly);
  ASSERT_EQ(F.Params.size(), 2u);
  for (const MedVar &Param : F.Params)
    EXPECT_EQ(Param.RegOff, kNoParamReg);
  // A stub that takes its operands in registers keeps them, as Go 1.16's
  // panicIndex takes AX and CX: mov [rsp+8], rax; mov [rsp+16], rcx; ret.
  constexpr va_t Stub = kTextVA + 0x280;
  T.writeText(
      Stub, {0x48, 0x89, 0x44, 0x24, 0x08, 0x48, 0x89, 0x4C, 0x24, 0x10, 0xC3});
  EXPECT_FALSE(lift(T.Img, Stub).StackArgumentsOnly);

  // Call recovery passes the callee's arguments on the stack where its own
  // recovery found them there.
  // Its record's 24 bytes hold its result too, after the two it reads.
  const std::map<va_t, int> StackCallees{{Add, -1}};
  std::map<va_t, int> TotalArity{{Add, static_cast<int>(F.Params.size())}};
  MedFunc G = lift(T.Img, Caller);
  recoverCallAbi(G, T.Img.Arch, {}, &T.Img, nullptr, &TotalArity, nullptr,
                 nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                 nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                 &StackCallees);
  ASSERT_EQ(G.CallInfos.size(), 1u);
  const std::vector<MedVar> &Arguments = G.CallInfos[0].Args;
  ASSERT_EQ(Arguments.size(), 2u);
  EXPECT_TRUE(G.CallInfos[0].ArgumentsFromPrototype);
  EXPECT_TRUE(Arguments[0].isConst() && Arguments[0].ConstVal == 1);
  EXPECT_TRUE(Arguments[1].isConst() && Arguments[1].ConstVal == 2);
}

TEST(GoStackArguments, ACalleeWithoutASizeTakesWhatItReads) {
  // ArgumentsAndTheirCallsAreOnTheStack's main.add and its caller, where
  // main.add's pclntab record gives no argument size, as Go assembly declared
  // `$0` (Windows' runtime.externalthreadhandler) has none.  Its own recovery
  // still finds two stack parameters; a call read by Win64's or System V's
  // positions passed none of the words the caller stores.  A block NeverD
  // splits off a function's interior has no record at all, and the frame it
  // reads is its function's own: its calls keep the platform's reading.
  constexpr va_t Add = kTextVA + 0x100;
  constexpr va_t Caller = kTextVA + 0x200;
  for (const BinaryFormat Format : {BinaryFormat::ELF, BinaryFormat::COFF})
    for (const bool Recorded : {true, false}) {
      SCOPED_TRACE(static_cast<unsigned>(Format) * 2 + Recorded);
      GoTestImage T;
      T.Img.Format = Format;
      T.writeText(Add, {0x48, 0x8B, 0x44, 0x24, 0x08, 0x48, 0x03, 0x44, 0x24,
                        0x10, 0x48, 0x89, 0x44, 0x24, 0x18, 0xC3});
      std::vector<uint8_t> Body = {0x48, 0x83, 0xEC, 0x20, 0x48, 0xC7, 0x04,
                                   0x24, 0x01, 0x00, 0x00, 0x00, 0x48, 0xC7,
                                   0x44, 0x24, 0x08, 0x02, 0x00, 0x00, 0x00};
      const std::vector<uint8_t> Call = makeCall(Caller + Body.size(), Add);
      Body.insert(Body.end(), Call.begin(), Call.end());
      Body.insert(Body.end(),
                  {0x48, 0x8B, 0x44, 0x24, 0x10, 0x48, 0x83, 0xC4, 0x20, 0xC3});
      T.writeText(Caller, Body);
      GoModuleInfo Module;
      Module.StackArguments = true;
      Module.GoCode = {{Add, kTextVA + 0x300}};
      Module.FrameArguments = {{Caller, 0}};
      if (Recorded)
        Module.FrameArguments.insert(Module.FrameArguments.begin(),
                                     {Add, std::nullopt});
      T.Img.ExceptionMetadata.GoModule = std::move(Module);
      ASSERT_FALSE(goFrameArgumentBytes(T.Img, Add));
      EXPECT_EQ(goRecordsFunction(T.Img, Add), Recorded);

      const MedFunc F = lift(T.Img, Add);
      EXPECT_TRUE(F.StackArgumentsOnly);
      ASSERT_EQ(F.Params.size(), 2u);
      const std::map<va_t, int> StackCallees{{Add, -1}};
      std::map<va_t, int> TotalArity{{Add, 2}};
      MedFunc G = lift(T.Img, Caller);
      recoverCallAbi(G, T.Img.Arch, {}, &T.Img, nullptr, &TotalArity, nullptr,
                     nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                     nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                     nullptr, &StackCallees);
      ASSERT_EQ(G.CallInfos.size(), 1u);
      EXPECT_EQ(G.CallInfos[0].ArgumentsFromPrototype, Recorded);
      if (!Recorded)
        continue;
      const std::vector<MedVar> &Arguments = G.CallInfos[0].Args;
      ASSERT_EQ(Arguments.size(), 2u);
      EXPECT_TRUE(Arguments[0].isConst() && Arguments[0].ConstVal == 1);
      EXPECT_TRUE(Arguments[1].isConst() && Arguments[1].ConstVal == 2);
    }
}

TEST(GoStackArguments, ARunOfTheCallerAlonePassesItsStackArguments) {
  // ArgumentsAndTheirCallsAreOnTheStack's main.add and its caller, run over
  // the caller alone, as `--func` and the GUI run one function.  The call
  // took main.add's summary, which counts its stack words after System V's
  // six register positions: `main_add(0, 0, 0, 0, 0, 0, 1, 2)`.
  constexpr va_t Add = kTextVA + 0x100;
  constexpr va_t Caller = kTextVA + 0x200;
  for (const bool Restricted : {false, true}) {
    SCOPED_TRACE(Restricted);
    GoTestImage T;
    T.writeText(Add, {0x48, 0x8B, 0x44, 0x24, 0x08, 0x48, 0x03, 0x44, 0x24,
                      0x10, 0x48, 0x89, 0x44, 0x24, 0x18, 0xC3});
    std::vector<uint8_t> Body = {0x48, 0x83, 0xEC, 0x20, 0x48, 0xC7, 0x04,
                                 0x24, 0x01, 0x00, 0x00, 0x00, 0x48, 0xC7,
                                 0x44, 0x24, 0x08, 0x02, 0x00, 0x00, 0x00};
    const std::vector<uint8_t> Call = makeCall(Caller + Body.size(), Add);
    Body.insert(Body.end(), Call.begin(), Call.end());
    Body.insert(Body.end(),
                {0x48, 0x8B, 0x44, 0x24, 0x10, 0x48, 0x83, 0xC4, 0x20, 0xC3});
    T.writeText(Caller, Body);
    GoModuleInfo Module;
    Module.StackArguments = true;
    Module.GoCode = {{Add, kTextVA + 0x300}};
    Module.FrameArguments = {{Add, 24}, {Caller, 0}};
    T.Img.ExceptionMetadata.GoModule = std::move(Module);
    for (const auto &[Entry, Name] :
         {std::pair{Add, "main.add"}, std::pair{Caller, "main.f"}}) {
      Symbol Function = Symbol::makeFunc(Entry);
      Function.Name = Name;
      T.Img.Symbols.push_back(std::move(Function));
    }

    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    if (Restricted)
      Options.OnlyFunctionEntries = {Caller};
    llvm::LLVMContext Context;
    const PipelineResult R = Pipeline().run(T.Img, Context, Options);
    ASSERT_TRUE(R.Success) << R.Error;
    const auto F = llvm::find_if(
        R.MedFuncs, [&](const MedFunc &M) { return M.Entry == Caller; });
    ASSERT_NE(F, R.MedFuncs.end());
    ASSERT_EQ(F->CallInfos.size(), 1u);
    const std::vector<MedVar> &Arguments = F->CallInfos[0].Args;
    ASSERT_EQ(Arguments.size(), 2u);
    EXPECT_TRUE(Arguments[0].isConst() && Arguments[0].ConstVal == 1);
    EXPECT_TRUE(Arguments[1].isConst() && Arguments[1].ConstVal == 2);
  }
}

TEST(GoStackArguments, AValueNothingNeedsTakesNoArgument) {
  // Registers a Go 1.16 function reads at its entry without taking an
  // argument there.  Read as register arguments, each put an unused parameter
  // first and moved every stack parameter one place, as in mallocgc(?, size,
  // typ, needzero):
  //   mallocgc's `sete r9b` keeps R9's upper bytes, which nothing reads:
  //     mov rax, [rsp+8]; cmp rax, [rsp+16]; sete r9b; movzx eax, r9b;
  //     mov [rsp+24], rax; ret
  //   Go's nil check reads AL only to set flags nothing reads:
  //     mov rbx, [rsp+8]; testb al, (rbx); mov rax, [rbx];
  //     mov [rsp+16], rax; ret
  //   a return reads RAX, where the stack ABI returns nothing:
  //     mov rcx, [rsp+8]; mov [rsp+16], rcx; ret
  //   memmove keeps RAX's upper bytes on its byte path and returns
  //   without writing RAX on another, where only its return reads it:
  //     mov rcx, [rsp+16]; test rcx, rcx; je L; mov rsi, [rsp+8];
  //     mov al, [rsi]; mov [rsp+24], al; L: ret
  //   `vzeroupper` keeps each XMM, as a register stays itself, and the stack
  //   ABI passes no vector argument:
  //     mov rax, [rsp+8]; vzeroupper; mov [rsp+16], rax; ret
  struct Shape {
    std::vector<uint8_t> Code;
    uint32_t FrameBytes;
    size_t Parameters;
  };
  const Shape Shapes[] = {
      {{0x48, 0x8B, 0x44, 0x24, 0x08, 0x48, 0x3B, 0x44, 0x24, 0x10, 0x41, 0x0F,
        0x94, 0xC1, 0x41, 0x0F, 0xB6, 0xC1, 0x48, 0x89, 0x44, 0x24, 0x18, 0xC3},
       24,
       2},
      {{0x48, 0x8B, 0x5C, 0x24, 0x08, 0x84, 0x03, 0x48, 0x8B, 0x03, 0x48, 0x89,
        0x44, 0x24, 0x10, 0xC3},
       16,
       1},
      {{0x48, 0x8B, 0x4C, 0x24, 0x08, 0x48, 0x89, 0x4C, 0x24, 0x10, 0xC3},
       16,
       1},
      {{0x48, 0x8B, 0x4C, 0x24, 0x10, 0x48, 0x85, 0xC9, 0x74, 0x0B, 0x48,
        0x8B, 0x74, 0x24, 0x08, 0x8A, 0x06, 0x88, 0x44, 0x24, 0x18, 0xC3},
       24,
       2},
      {{0x48, 0x8B, 0x44, 0x24, 0x08, 0xC5, 0xF8, 0x77, 0x48, 0x89, 0x44, 0x24,
        0x10, 0xC3},
       16,
       1},
  };
  for (const Shape &Case : Shapes) {
    SCOPED_TRACE(Case.Code.size());
    constexpr va_t Entry = kTextVA + 0x100;
    GoTestImage T;
    T.writeText(Entry, Case.Code);
    GoModuleInfo Module;
    Module.StackArguments = true;
    Module.GoCode = {{Entry, kTextVA + 0x300}};
    Module.FrameArguments = {{Entry, Case.FrameBytes}};
    T.Img.ExceptionMetadata.GoModule = std::move(Module);

    const MedFunc F = lift(T.Img, Entry);
    EXPECT_TRUE(F.StackArgumentsOnly);
    EXPECT_EQ(F.Params.size(), Case.Parameters);
    for (const MedVar &Param : F.Params)
      EXPECT_EQ(Param.RegOff, kNoParamReg);
  }
}

TEST(GoStackArguments, ARegisterStubTakesTheRegistersItPassesOn) {
  // Go 1.16's runtime.panicIndex: mov [rsp+8], rax; mov [rsp+16], rcx;
  // jmp runtime.goPanicIndex.  It takes x in AX and y in CX, which it stores
  // as the stack-ABI routine's arguments.  Read as System V's, its callers
  // passed (0, 0, 0, y); read as Win64's, only y.  A caller sets both:
  //   mov eax, 5; mov ecx, 9; call runtime.panicIndex; ret
  for (const BinaryFormat Format : {BinaryFormat::ELF, BinaryFormat::COFF}) {
    SCOPED_TRACE(static_cast<unsigned>(Format));
    constexpr va_t Stub = kTextVA + 0x100;
    constexpr va_t Half = kTextVA + 0x180;
    constexpr va_t Caller = kTextVA + 0x200;
    GoTestImage T;
    T.Img.Format = Format;
    std::vector<uint8_t> StubBody = {0x48, 0x89, 0x44, 0x24, 0x08,
                                     0x48, 0x89, 0x4C, 0x24, 0x10};
    const std::vector<uint8_t> Jump = {0xE9, 0x00, 0x00, 0x00, 0x00};
    StubBody.insert(StubBody.end(), Jump.begin(), Jump.end());
    const int32_t Rel = static_cast<int32_t>(Half - (Stub + StubBody.size()));
    std::memcpy(StubBody.data() + StubBody.size() - 4, &Rel, 4);
    T.writeText(Stub, StubBody);
    // goPanicIndex reads its two arguments: mov rax, [rsp+8];
    // add rax, [rsp+16]; ret.
    T.writeText(Half, {0x48, 0x8B, 0x44, 0x24, 0x08, 0x48, 0x03, 0x44, 0x24,
                       0x10, 0xC3});
    std::vector<uint8_t> CallerBody = {0xB8, 0x05, 0x00, 0x00, 0x00,
                                       0xB9, 0x09, 0x00, 0x00, 0x00};
    const std::vector<uint8_t> Call =
        makeCall(Caller + CallerBody.size(), Stub);
    CallerBody.insert(CallerBody.end(), Call.begin(), Call.end());
    CallerBody.push_back(0xC3);
    T.writeText(Caller, CallerBody);
    GoModuleInfo Module;
    Module.StackArguments = true;
    Module.GoCode = {{Stub, kTextVA + 0x300}};
    Module.FrameArguments = {{Stub, 16}, {Half, 16}, {Caller, 0}};
    T.Img.ExceptionMetadata.GoModule = std::move(Module);
    // The pclntab names each routine, so the stub's jump is a tail call.
    for (const auto &[Entry, Name] : {std::pair{Stub, "runtime.panicIndex"},
                                      std::pair{Half, "runtime.goPanicIndex"},
                                      std::pair{Caller, "main.f"}}) {
      Symbol Function = Symbol::makeFunc(Entry);
      Function.Name = Name;
      T.Img.Symbols.push_back(std::move(Function));
    }

    const LowFunc StubLow = build(T.Img, Stub);
    const std::optional<FixedIntegerCallArguments> Arguments =
        goRegisterStubArguments(T.Img, Stub, StubLow.Blocks);
    ASSERT_TRUE(Arguments.has_value());
    ASSERT_EQ(Arguments->Parameters.size(), 2u);
    EXPECT_EQ(Arguments->Parameters[0].Register, x86reg::RAX);
    EXPECT_EQ(Arguments->Parameters[1].Register, x86reg::RCX);

    // Its summary reads exactly those, whatever goPanicIndex reads, and
    // names them for its callers.
    const LocalRegisterEffect Effect =
        localRegisterEffect(T.Img, CallRegisterBodyView(StubLow), nullptr,
                            nullptr, nullptr, nullptr);
    ASSERT_TRUE(Effect.GoFixedArguments.has_value());
    EXPECT_EQ(*Effect.GoFixedArguments, *Arguments);
    ASSERT_TRUE(Effect.GoArgumentReads.has_value());
    GPRReadWidths Reads{};
    Reads[x86reg::RAX / 8] = 8;
    Reads[x86reg::RCX / 8] = 8;
    EXPECT_EQ(*Effect.GoArgumentReads, Reads);

    const MedFunc F = lift(T.Img, Stub);
    ASSERT_EQ(F.IntegerArgumentRegisters,
              std::vector<uint64_t>({x86reg::RAX, x86reg::RCX}));
    ASSERT_EQ(F.Params.size(), 2u);
    EXPECT_EQ(F.Params[0].RegOff, x86reg::RAX);
    EXPECT_EQ(F.Params[1].RegOff, x86reg::RCX);
    EXPECT_TRUE(F.ExactRegisterParameters);

    // The caller binds both setups before SSA, in the stub's order, beside
    // the summary that reads AX, a register no platform position names.
    const std::map<va_t, GPRReadWidths> Summaries{{Stub, Reads}};
    const std::map<va_t, FixedIntegerCallArguments> Fixed{{Stub, *Arguments}};
    LowToMedConverter Converter;
    Converter.setBinaryImage(&T.Img);
    Converter.setCallEntryReadGPRs(&Summaries);
    Converter.setCallEntryIntegerArguments(&Fixed);
    MedFunc G = Converter.convert(build(T.Img, Caller), T.Img.Arch, Format);
    std::map<va_t, int> RegArity{{Stub, 2}}, TotalArity{{Stub, 2}};
    const std::map<va_t, std::vector<uint64_t>> IntRegs{
        {Stub, F.IntegerArgumentRegisters}};
    recoverCallAbi(G, T.Img.Arch, {}, &T.Img, &RegArity, &TotalArity, nullptr,
                   nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                   &IntRegs);
    ASSERT_EQ(G.CallInfos.size(), 1u);
    const std::vector<MedVar> &Passed = G.CallInfos[0].Args;
    ASSERT_EQ(Passed.size(), 2u);
    EXPECT_TRUE(Passed[0].isConst() && Passed[0].ConstVal == 5);
    EXPECT_TRUE(Passed[1].isConst() && Passed[1].ConstVal == 9);
  }
}

TEST(GoStackArguments, ARunOfTheCallerAloneBindsAListedStubsRegisters) {
  // ARegisterStubTakesTheRegistersItPassesOn's image, with panicIndex and its
  // half listed as never returning (GoModuleInfo::NoReturnEntries).  A call
  // into a listed routine adds no callee to the summaries, so a run of the
  // caller alone summarized no stub, and the call passed no argument.
  constexpr va_t Stub = kTextVA + 0x100;
  constexpr va_t Half = kTextVA + 0x180;
  constexpr va_t Caller = kTextVA + 0x200;
  for (const bool Restricted : {false, true}) {
    SCOPED_TRACE(Restricted);
    GoTestImage T;
    std::vector<uint8_t> StubBody = {0x48, 0x89, 0x44, 0x24, 0x08,
                                     0x48, 0x89, 0x4C, 0x24, 0x10,
                                     0xE9, 0x00, 0x00, 0x00, 0x00};
    const int32_t Rel = static_cast<int32_t>(Half - (Stub + StubBody.size()));
    std::memcpy(StubBody.data() + StubBody.size() - 4, &Rel, 4);
    T.writeText(Stub, StubBody);
    T.writeText(Half, {0x48, 0x8B, 0x44, 0x24, 0x08, 0x48, 0x03, 0x44, 0x24,
                       0x10, 0xC3});
    std::vector<uint8_t> CallerBody = {0xB8, 0x05, 0x00, 0x00, 0x00,
                                       0xB9, 0x09, 0x00, 0x00, 0x00};
    const std::vector<uint8_t> Call =
        makeCall(Caller + CallerBody.size(), Stub);
    CallerBody.insert(CallerBody.end(), Call.begin(), Call.end());
    CallerBody.push_back(0xC3);
    T.writeText(Caller, CallerBody);
    GoModuleInfo Module;
    Module.StackArguments = true;
    Module.GoCode = {{Stub, kTextVA + 0x300}};
    Module.FrameArguments = {{Stub, 16}, {Half, 16}, {Caller, 0}};
    Module.NoReturnEntries = {Stub, Half};
    T.Img.ExceptionMetadata.GoModule = std::move(Module);
    for (const auto &[Entry, Name] : {std::pair{Stub, "runtime.panicIndex"},
                                      std::pair{Half, "runtime.goPanicIndex"},
                                      std::pair{Caller, "main.f"}}) {
      Symbol Function = Symbol::makeFunc(Entry);
      Function.Name = Name;
      T.Img.Symbols.push_back(std::move(Function));
    }

    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    if (Restricted)
      Options.OnlyFunctionEntries = {Caller};
    llvm::LLVMContext Context;
    const PipelineResult R = Pipeline().run(T.Img, Context, Options);
    ASSERT_TRUE(R.Success) << R.Error;
    const std::map<va_t, FixedIntegerCallArguments> &Fixed =
        R.CallEntryIntegerArguments.get();
    const auto Operands = Fixed.find(Stub);
    ASSERT_NE(Operands, Fixed.end());
    ASSERT_EQ(Operands->second.Parameters.size(), 2u);
    EXPECT_EQ(Operands->second.Parameters[0].Register, x86reg::RAX);
    EXPECT_EQ(Operands->second.Parameters[1].Register, x86reg::RCX);
    const auto F = llvm::find_if(
        R.MedFuncs, [&](const MedFunc &M) { return M.Entry == Caller; });
    ASSERT_NE(F, R.MedFuncs.end());
    const MedOp *Bound = nullptr;
    for (const MedBlock &Block : F->Blocks)
      for (const MedOp &Op : Block.Ops)
        if (Op.Opcode == NdOp::CALL && Op.NumInputs >= 1 &&
            Op.Inputs[0].isConst() && Op.Inputs[0].ConstVal == Stub)
          Bound = &Op;
    ASSERT_NE(Bound, nullptr);
    ASSERT_TRUE(Bound->IntegerCallSignature);
    ASSERT_EQ(Bound->NumInputs, 3u);
    EXPECT_TRUE(Bound->Inputs[1].isConst() && Bound->Inputs[1].ConstVal == 5);
    EXPECT_TRUE(Bound->Inputs[2].isConst() && Bound->Inputs[2].ConstVal == 9);
  }
}

TEST(GoStackArguments, OnlyAStubPassesItsRegistersOn) {
  // Two routines that read registers at their entry but are no stub.
  // memeqbody takes SI, DI, BX and AX and has no argument words:
  //   cmp rsi, rdi; mov rax, rbx; ret
  // A memequal shape stores and compares on two paths before it passes its
  // registers on:
  //   mov [rsp+8], rax; test rcx, rcx; je L; mov [rsp+16], rcx;
  //   L: jmp half
  constexpr va_t Body = kTextVA + 0x100;
  constexpr va_t Equal = kTextVA + 0x140;
  constexpr va_t Half = kTextVA + 0x180;
  GoTestImage T;
  T.writeText(Body, {0x48, 0x39, 0xFE, 0x48, 0x89, 0xD8, 0xC3});
  std::vector<uint8_t> EqualBody = {0x48, 0x89, 0x44, 0x24, 0x08, 0x48, 0x85,
                                    0xC9, 0x74, 0x05, 0x48, 0x89, 0x4C, 0x24,
                                    0x10, 0xE9, 0x00, 0x00, 0x00, 0x00};
  const int32_t Rel = static_cast<int32_t>(Half - (Equal + EqualBody.size()));
  std::memcpy(EqualBody.data() + EqualBody.size() - 4, &Rel, 4);
  T.writeText(Equal, EqualBody);
  T.writeText(Half, {0x48, 0x8B, 0x44, 0x24, 0x08, 0xC3});
  GoModuleInfo Module;
  Module.StackArguments = true;
  Module.GoCode = {{Body, kTextVA + 0x300}};
  Module.FrameArguments = {{Body, 0}, {Equal, 16}, {Half, 16}};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  for (const auto &[Entry, Name] :
       {std::pair{Body, "memeqbody"}, std::pair{Equal, "runtime.memequal"},
        std::pair{Half, "runtime.half"}}) {
    Symbol Function = Symbol::makeFunc(Entry);
    Function.Name = Name;
    T.Img.Symbols.push_back(std::move(Function));
  }
  for (const va_t Entry : {Body, Equal}) {
    SCOPED_TRACE(Entry);
    const LowFunc Low = build(T.Img, Entry);
    EXPECT_FALSE(goRegisterStubArguments(T.Img, Entry, Low.Blocks));
    EXPECT_FALSE(localRegisterEffect(T.Img, CallRegisterBodyView(Low), nullptr,
                                     nullptr, nullptr, nullptr)
                     .GoFixedArguments);
  }
}

TEST(GoNoReturn, ACallToGopanicHasNoContinuation) {
  // main.h: test eax, eax; jz L; call runtime.gopanic; L: ret.  gopanic's
  // body returns, as its store to nil and return do, but the compiler laid
  // L after the call as another block, which the call never reaches.
  constexpr va_t Panicking = kTextVA + 0x100, Gopanic = kTextVA + 0x200;
  GoTestImage T;
  T.writeText(Panicking, {0x85, 0xC0, 0x74, 0x05});
  T.writeText(Panicking + 4, makeCall(Panicking + 4, Gopanic));
  T.writeText(Panicking + 9, {0xC3});
  T.writeText(Gopanic, {0xC3});
  GoModuleInfo Module;
  Module.RegisterArguments = true;
  Module.GoCode = {{Panicking, kTextVA + 0x300}};
  Module.NoReturnEntries = {Gopanic};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  const LowFunc F = build(T.Img, Panicking);
  for (const LowBlock &Block : F.Blocks)
    if (Block.StartAddr <= Panicking + 4 && Panicking + 4 < Block.EndAddr)
      for (const int Succ : Block.Succs)
        for (const LowBlock &Next : F.Blocks)
          if (Next.Id == Succ)
            EXPECT_NE(Next.StartAddr, Panicking + 9);
}

/// main.b: call runtime.panicBounds, whose PCDATA_PanicBounds value for the
/// call is \p Value (abi.BoundsEncode).
BinaryImage panicBoundsImage(int32_t Value) {
  constexpr va_t Caller = kTextVA + 0x100, PanicBounds = kTextVA + 0x200;
  GoTestImage T;
  T.writeText(Caller, makeCall(Caller, PanicBounds));
  T.writeText(Caller + 5, {0xC3});
  T.writeText(PanicBounds, {0xC3});
  GoModuleInfo Module;
  Module.RegisterArguments = true;
  Module.GoCode = {{Caller, kTextVA + 0x300}};
  Module.PanicBoundsEntry = PanicBounds;
  Module.PanicBounds = {{{Caller, Caller + 5}, Value}};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  return std::move(T.Img);
}

TEST(GoPanicBounds, DecodeTheCompilersEncoding) {
  // abi.BoundsEncode(BoundsIndex, signed, CX, DX) and
  // abi.BoundsEncode(BoundsSliceAlen, unsigned, 5, BX).
  const BinaryImage Index = panicBoundsImage(2439);
  const auto A = goBoundsFailureAt(Index, kTextVA + 0x100, kTextVA + 0x200);
  ASSERT_TRUE(A.has_value());
  EXPECT_EQ(A->Code, 0u);
  EXPECT_TRUE(A->Signed);
  EXPECT_TRUE(A->Index.Register);
  EXPECT_EQ(A->Index.Value, x86reg::RCX);
  EXPECT_TRUE(A->Bound.Register);
  EXPECT_EQ(A->Bound.Value, x86reg::RDX);
  const BinaryImage Slice = panicBoundsImage(3655);
  const auto B = goBoundsFailureAt(Slice, kTextVA + 0x100, kTextVA + 0x200);
  ASSERT_TRUE(B.has_value());
  EXPECT_EQ(B->Code, 1u);
  EXPECT_FALSE(B->Signed);
  EXPECT_FALSE(B->Index.Register);
  EXPECT_EQ(B->Index.Value, 5u);
  EXPECT_EQ(B->Bound.Value, x86reg::RBX);
  // panicBounds saves neither SP (4) nor g (14), so no entry names them:
  // abi.BoundsEncode(BoundsIndex, unsigned, SP, DX).
  EXPECT_FALSE(
      goBoundsFailureAt(panicBoundsImage((1 | 2 | 4 << 3 | 2 << 7) * 9),
                        kTextVA + 0x100, kTextVA + 0x200));
  // Another callee, or a call no range covers, reports nothing.
  EXPECT_FALSE(goBoundsFailureAt(Index, kTextVA + 0x100, kTextVA + 0x280));
  EXPECT_FALSE(goBoundsFailureAt(Index, kTextVA + 0x105, kTextVA + 0x200));
}

TEST(GoPanicBounds, ACallPassesItsIndexAndBound) {
  const BinaryImage Img = panicBoundsImage(2439);
  const MedFunc F = lift(Img, kTextVA + 0x100);
  const MedOp *Call = onlyCall(F);
  ASSERT_NE(Call, nullptr);
  ASSERT_EQ(Call->NumInputs, 3u);
  EXPECT_EQ(Call->Inputs[1].RegOff, x86reg::RCX);
  EXPECT_EQ(Call->Inputs[2].RegOff, x86reg::RDX);
  // A constant index is passed as one.
  const MedOp *Constant =
      onlyCall(lift(panicBoundsImage(3655), kTextVA + 0x100));
  ASSERT_NE(Constant, nullptr);
  ASSERT_EQ(Constant->NumInputs, 3u);
  EXPECT_EQ(Constant->Inputs[1].Kind, MedVar::Const);
  EXPECT_EQ(Constant->Inputs[1].ConstVal, 5u);
  EXPECT_EQ(Constant->Inputs[2].RegOff, x86reg::RBX);
}

TEST(GoPanicBounds, AnARM64CallMovesItsOperandsIntoTheParameters) {
  // main.b: nop; bl runtime.panicBounds; ret, whose PCDATA_PanicBounds
  // value is abi.BoundsEncode(BoundsIndex, signed, R3, R5).  An arm64 call
  // takes the registers set before it, so R3 and R5 move into X0 and X1
  // first.  The nop keeps the call apart from the entry's own copies.
  constexpr va_t Caller = kTextVA + 0x100, PanicBounds = kTextVA + 0x200;
  constexpr va_t Site = Caller + 4;
  GoTestImage T(Arch::AArch64);
  const uint32_t Call =
      0x94000000u | (((PanicBounds - Site) >> 2) & 0x3FFFFFFu);
  T.writeText(Caller, words({0xD503201F, Call, 0xD65F03C0}));
  // The routine saves X8 among the registers that may hold the index; that
  // is no indirect-result pointer: stp x8, x9, [sp, #-16]!; ldp ...; ret.
  T.writeText(PanicBounds, words({0xA9BF27E8, 0xA8C127E8, 0xD65F03C0}));
  GoModuleInfo Module;
  Module.RegisterArguments = true;
  Module.GoCode = {{Caller, kTextVA + 0x300}};
  Module.GoAssembly = {{PanicBounds, PanicBounds + 12}};
  Module.PanicBoundsEntry = PanicBounds;
  Module.PanicBounds = {{{Site, Site + 4}, (1 | 2 | 4 | 3 << 3 | 5 << 7) * 9}};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  const auto Failure = goBoundsFailureAt(T.Img, Site, PanicBounds);
  ASSERT_TRUE(Failure.has_value());
  EXPECT_EQ(Failure->Index.Value, a64reg::X3);
  EXPECT_EQ(Failure->Bound.Value, a64reg::X5);
  const MedFunc F = lift(T.Img, Caller);
  // X0 and X1 take R3 and R5 before the call, directly or through the
  // temporaries that keep a swapped pair apart.
  std::map<std::pair<int, int>, uint64_t> TempSource;
  std::map<uint64_t, uint64_t> Source;
  for (const MedBlock &Block : F.Blocks)
    for (const MedOp &Op : Block.Ops)
      if (Op.Opcode == NdOp::COPY && Op.Addr == Site && Op.NumInputs == 1 &&
          Op.Inputs[0].Kind == MedVar::Reg) {
        if (Op.Output.Kind == MedVar::Temp)
          TempSource[{Op.Output.Id, Op.Output.SSAVer}] = Op.Inputs[0].RegOff;
        else if (Op.Output.Kind == MedVar::Reg)
          Source[Op.Output.RegOff] = Op.Inputs[0].RegOff;
      }
  for (const MedBlock &Block : F.Blocks)
    for (const MedOp &Op : Block.Ops)
      if (Op.Opcode == NdOp::COPY && Op.Addr == Site && Op.NumInputs == 1 &&
          Op.Output.Kind == MedVar::Reg && Op.Inputs[0].Kind == MedVar::Temp)
        if (const auto It =
                TempSource.find({Op.Inputs[0].Id, Op.Inputs[0].SSAVer});
            It != TempSource.end())
          Source[Op.Output.RegOff] = It->second;
  EXPECT_EQ(Source[a64reg::X0], a64reg::X3);
  EXPECT_EQ(Source[a64reg::X1], a64reg::X5);
  // The routine takes exactly those two.
  const MedFunc Routine = lift(T.Img, PanicBounds);
  ASSERT_EQ(Routine.Params.size(), 2u);
  EXPECT_EQ(Routine.Params[0].RegOff, a64reg::X0);
  EXPECT_EQ(Routine.Params[1].RegOff, a64reg::X1);
  EXPECT_TRUE(Routine.ExactRegisterParameters);
}

TEST(GoPanicBounds, AWholeRunReadsABoundsCheckAsARunOfOneDoes) {
  // runtime.panicBounds saves every register its PCDATA_PanicBounds entry
  // may name, so its own summary reads them all.  A run read the entry at
  // a call only where the caller was a callee it took in, not where it was
  // one of the run's own functions, as every function of a whole image is:
  // main.f, whose failed bounds check names RBX and a constant, read RCX,
  // RDX, RSI, RDI and R8-R11 too, and main.g's call passed them.
  //   main.f(p *[4]int, i int) int: cmp rbx, 4; jae 1f
  //                         mov rax, [rax+rbx*8]; ret; 1: call panicBounds
  //   main.g(p *[4]int) int: push rbp; mov rbp, rsp; mov ebx, 2; call main.f
  //                         inc rax; pop rbp; ret
  //   runtime.panicBounds:  push rbp; mov rbp, rsp; sub rsp, 128
  //                         mov [rsp+8*k], each of RAX..R11; ud2
  constexpr va_t F = kTextVA + 0x100, G = kTextVA + 0x140;
  constexpr va_t PanicBounds = kTextVA + 0x180;
  GoTestImage T;
  T.writeText(
      F,
      {0x48, 0x83, 0xFB, 0x04, 0x73, 0x05, 0x48, 0x8B, 0x04, 0xD8, 0xC3, 0xE8,
       0x70, 0x00, 0x00, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
       0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
       0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
       0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
       0xCC, 0xCC, 0xCC, 0xCC, 0x55, 0x48, 0x89, 0xE5, 0xBB, 0x02, 0x00, 0x00,
       0x00, 0xE8, 0xB2, 0xFF, 0xFF, 0xFF, 0x48, 0xFF, 0xC0, 0x5D, 0xC3, 0xCC,
       0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
       0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
       0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
       0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0x55, 0x48, 0x89, 0xE5,
       0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00, 0x48, 0x89, 0x04, 0x24, 0x48,
       0x89, 0x4C, 0x24, 0x08, 0x48, 0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x5C,
       0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x48, 0x89, 0x7C, 0x24, 0x28,
       0x4C, 0x89, 0x44, 0x24, 0x30, 0x4C, 0x89, 0x4C, 0x24, 0x38, 0x4C, 0x89,
       0x54, 0x24, 0x40, 0x4C, 0x89, 0x5C, 0x24, 0x48, 0x0F, 0x0B});
  for (const auto &[Entry, Name] :
       {std::pair{F, "main.f"}, std::pair{G, "main.g"},
        std::pair{PanicBounds, "runtime.panicBounds"}}) {
    Symbol Function = Symbol::makeFunc(Entry);
    Function.Name = Name;
    T.Img.Symbols.push_back(std::move(Function));
  }
  GoModuleInfo Module;
  Module.PclnTabMagic = 0xFFFFFFF1u;
  Module.RegisterArguments = true;
  Module.GoCode = {{F, PanicBounds}};
  Module.GoAssembly = {{PanicBounds, PanicBounds + 0x40}};
  Module.PanicBoundsEntry = PanicBounds;
  // abi.BoundsEncode(BoundsIndex, signed, BX, 4).
  Module.PanicBounds = {{{F + 0xB, F + 0x10}, (1 | 4 | 3 << 3 | 4 << 7) * 9}};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  for (const bool Whole : {true, false}) {
    SCOPED_TRACE(Whole);
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    if (!Whole)
      Options.OnlyFunctionEntries = {G};
    llvm::LLVMContext Context;
    const PipelineResult R = Pipeline().run(T.Img, Context, Options);
    ASSERT_TRUE(R.Success) << R.Error;
    const auto Function = [&](va_t Entry) -> const MedFunc * {
      const auto It = llvm::find_if(
          R.MedFuncs, [&](const MedFunc &M) { return M.Entry == Entry; });
      return It == R.MedFuncs.end() ? nullptr : &*It;
    };
    const MedFunc *Caller = Function(G);
    ASSERT_NE(Caller, nullptr);
    const MedOp *Call = onlyCall(*Caller);
    ASSERT_NE(Call, nullptr);
    EXPECT_EQ(Call->CalleeRegisterArgs, 2) << int(Call->CalleeRegisterArgs);
    EXPECT_EQ(Call->NumInputs, 3u);
    if (Whole) {
      const MedFunc *Callee = Function(F);
      ASSERT_NE(Callee, nullptr);
      EXPECT_EQ(Callee->Params.size(), 2u);
    }
  }
}

TEST(GoRegisterArguments, ARunOfTheCallerAloneSummarizesAListedRoutine) {
  // registerArgumentImage's main.f, listed as never returning as
  // runtime.throw is (GoModuleInfo::NoReturnEntries).  A call into a listed
  // routine added no callee to the summaries, so a run of main.g alone had no
  // summary of main.f, and its call took every register main.g had set in
  // Go's order: `runtime_throw(s, n, ...)` took seven.  Its stack-growth
  // path's spills give it exactly RAX and RBX in either run.
  for (const bool Restricted : {false, true}) {
    SCOPED_TRACE(Restricted);
    BinaryImage Img = registerArgumentImage();
    Img.ExceptionMetadata.GoModule->NoReturnEntries = {GoCallee};
    for (const auto &[Entry, Name] :
         {std::pair{GoCallee, "main.f"}, std::pair{GoCaller, "main.g"},
          std::pair{Morestack, "runtime.morestack_noctxt"}}) {
      Symbol Function = Symbol::makeFunc(Entry);
      Function.Name = Name;
      Img.Symbols.push_back(std::move(Function));
    }
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    if (Restricted)
      Options.OnlyFunctionEntries = {GoCaller};
    llvm::LLVMContext Context;
    const PipelineResult R = Pipeline().run(Img, Context, Options);
    ASSERT_TRUE(R.Success) << R.Error;
    const std::map<va_t, GPRReadWidths> &Reads = R.CallEntryReadGPRs.get();
    const auto Summary = Reads.find(GoCallee);
    ASSERT_NE(Summary, Reads.end());
    GPRReadWidths Expected{};
    Expected[x86reg::RAX / 8] = 8;
    Expected[x86reg::RBX / 8] = 8;
    EXPECT_EQ(Summary->second, Expected);
  }
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

TEST(GoAssembly, AnARM64RoutineRunInPlaceTakesTheRegistersItReads) {
  // Reflection's register transfers and Duff's device run in their caller's
  // place.  Standing alone, each takes the registers its body reads before
  // it writes them: on arm64 that is X20, which neither Go's internal ABI
  // nor AAPCS64 passes an argument in, so each printed it as a register
  // nothing defines.
  //   runtime.spillArgs:   stp x0, x1, [x20]; str x9, [x20, #72]; ret
  //   runtime.unspillArgs: ldp x0, x1, [x20]; ret
  //   runtime.duffzero:    stp xzr, xzr, [x20], #16 (twice); ret
  constexpr va_t Spill = kTextVA + 0x100;
  constexpr va_t Unspill = kTextVA + 0x140;
  constexpr va_t Zero = kTextVA + 0x180;
  GoTestImage T(Arch::AArch64);
  T.writeText(Spill, words({0xA9000680, 0xF9002689, 0xD65F03C0}));
  T.writeText(Unspill, words({0xA9400680, 0xD65F03C0}));
  T.writeText(Zero, words({0xA8817E9F, 0xA8817E9F, 0xD65F03C0}));
  for (const auto &[Entry, Name] : {std::pair{Spill, "runtime.spillArgs"},
                                    std::pair{Unspill, "runtime.unspillArgs"},
                                    std::pair{Zero, "runtime.duffzero"}}) {
    Symbol Function = Symbol::makeFunc(Entry);
    Function.Name = Name;
    T.Img.Symbols.push_back(std::move(Function));
  }
  GoModuleInfo Module;
  Module.PclnTabMagic = 0xFFFFFFF1u;
  Module.RegisterArguments = true;
  Module.GoCode = {{kTextVA, kTextVA + 0x400}};
  Module.GoAssembly = {
      {Spill, Spill + 0x40}, {Unspill, Unspill + 0x40}, {Zero, Zero + 0x40}};
  Module.RegisterArgumentTransfers = {{Spill, Spill + 0x40},
                                      {Unspill, Unspill + 0x40}};
  Module.DuffDevices = {{Zero, Zero + 0x40}};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);

  const auto Registers = [&](va_t Entry) {
    std::vector<uint64_t> Found;
    if (const auto Operands = goInlinedRoutineOperands(T.Img, Entry))
      for (const auto &Parameter : Operands->Parameters)
        Found.push_back(Parameter.Register);
    return Found;
  };
  // The platform's registers first, then Go's, then the others'.
  EXPECT_EQ(Registers(Spill), (std::vector<uint64_t>{a64reg::X0, a64reg::X1,
                                                     a64reg::X9, a64reg::X20}));
  EXPECT_EQ(Registers(Unspill), (std::vector<uint64_t>{a64reg::X20}));
  EXPECT_EQ(Registers(Zero), (std::vector<uint64_t>{a64reg::X20}));
  // A point inside Duff's device begins a routine of its own.
  EXPECT_EQ(Registers(Zero + 4), (std::vector<uint64_t>{a64reg::X20}));
  EXPECT_FALSE(goInlinedRoutineOperands(T.Img, kTextVA + 0x300).has_value());

  const MedFunc Stores = lift(T.Img, Spill);
  ASSERT_EQ(Stores.Params.size(), 4u);
  EXPECT_EQ(Stores.Params[3].RegOff, a64reg::X20);
  EXPECT_TRUE(Stores.ExactRegisterParameters);

  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  llvm::LLVMContext Context;
  const PipelineResult R = Pipeline().run(T.Img, Context, Options);
  ASSERT_TRUE(R.Success) << R.Error;
  EXPECT_EQ(R.MedIRVerifierFailures, 0u);
  for (const char *Name :
       {"runtime_spillArgs", "runtime_unspillArgs", "runtime_duffzero"}) {
    const std::string Text = highCBody(R, T.Img, Name);
    ASSERT_FALSE(Text.empty()) << Name;
    EXPECT_EQ(Text.find("__builtin_trap"), std::string::npos) << Text;
  }
  EXPECT_NE(highCBody(R, T.Img, "runtime_spillArgs").find("*(int64_t *)arg3"),
            std::string::npos)
      << highCBody(R, T.Img, "runtime_spillArgs");
}

TEST(GoAssembly, AnX64RoutineRunInPlaceTakesItsSummarysRegisters) {
  // x86-64 routines take the registers their summaries read
  // (goEntryRegisterOperands), so the body gives none.
  //   runtime.spillArgs: mov [r12], rax; ret
  constexpr va_t Spill = kTextVA + 0x100;
  GoTestImage T(Arch::X64);
  T.writeText(Spill, {0x49, 0x89, 0x04, 0x24, 0xC3});
  GoModuleInfo Module;
  Module.PclnTabMagic = 0xFFFFFFF1u;
  Module.RegisterArguments = true;
  Module.GoCode = {{kTextVA, kTextVA + 0x400}};
  Module.GoAssembly = {{Spill, Spill + 0x40}};
  Module.RegisterArgumentTransfers = {{Spill, Spill + 0x40}};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  EXPECT_FALSE(goInlinedRoutineOperands(T.Img, Spill).has_value());
}

TEST(GoAssembly, GoCodeHasNoVariadicPrologue) {
  // Go passes a variadic argument as a slice, so no Go function has the
  // platform's variadic prologue.  arm64's runtime.asyncPreempt keeps X0
  // to X7 and V0 to V7 for the code it interrupts, as an AAPCS64 variadic
  // function fills its register save area with both argument files:
  //   stur x30, [sp, #-0xf0]; sub sp, sp, #0xf0
  //   stp x0, x1, [sp, #8]; stp x2, x3, [sp, #0x18]
  //   stp x4, x5, [sp, #0x28]; stp x6, x7, [sp, #0x38]
  //   stp q0, q1, [sp, #0x50]; stp q2, q3, [sp, #0x70]
  //   stp q4, q5, [sp, #0x90]; stp q6, q7, [sp, #0xb0]
  //   add sp, sp, #0xf0; ret
  constexpr va_t Preempt = kTextVA + 0x100;
  GoTestImage T(Arch::AArch64);
  T.writeText(Preempt, words({0xF81103FE, 0xD103C3FF, 0xA90087E0, 0xA9018FE2,
                              0xA90297E4, 0xA9039FE6, 0xAD0287E0, 0xAD038FE2,
                              0xAD0497E4, 0xAD059FE6, 0x9103C3FF, 0xD65F03C0}));
  // Read as the platform's code, the function is variadic.
  EXPECT_TRUE(lift(T.Img, Preempt).IsVariadic);
  GoModuleInfo Module;
  Module.PclnTabMagic = 0xFFFFFFF1u;
  Module.RegisterArguments = true;
  Module.GoCode = {{Preempt, Preempt + 0x40}};
  Module.GoAssembly = {{Preempt, Preempt + 0x40}};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  EXPECT_FALSE(lift(T.Img, Preempt).IsVariadic);
}

TEST(GoAssembly, ACallToCodeTheGoToolchainDidNotBuildFollowsThePlatform) {
  // A routine the pclntab does not record is none the Go toolchain built:
  // a libc routine a Darwin trampoline reaches through its stub.  It keeps
  // the registers the platform's convention keeps, and the trampoline
  // holds its argument block in X19 across the call.  As a call into Go
  // code, which keeps no register, the store through X19 was unknown.
  //   runtime.tramp: stp x29, x30, [sp, #-16]!; mov x19, x0; ldr x0, [x19]
  //                  bl helper; str x0, [x19, #8]
  //                  ldp x29, x30, [sp], #16; ret
  //   helper:        add x0, x0, #1; ret
  constexpr va_t Tramp = kTextVA + 0x100;
  constexpr va_t Helper = kTextVA + 0x200;
  constexpr va_t Site = Tramp + 12;
  GoTestImage T(Arch::AArch64);
  const uint32_t Call = 0x94000000u | (((Helper - Site) >> 2) & 0x3FFFFFFu);
  T.writeText(Tramp, words({0xA9BF7BFD, 0xAA0003F3, 0xF9400260, Call,
                            0xF9000660, 0xA8C17BFD, 0xD65F03C0}));
  T.writeText(Helper, words({0x91000400, 0xD65F03C0}));
  for (const auto &[Entry, Name] :
       {std::pair{Tramp, "runtime.tramp"}, std::pair{Helper, "helper"}}) {
    Symbol Function = Symbol::makeFunc(Entry);
    Function.Name = Name;
    T.Img.Symbols.push_back(std::move(Function));
  }
  GoModuleInfo Module;
  Module.PclnTabMagic = 0xFFFFFFF1u;
  Module.RegisterArguments = true;
  Module.GoCode = {{Tramp, Tramp + 0x40}};
  Module.GoAssembly = {{Tramp, Tramp + 0x40}};
  Module.FrameArguments = {{Tramp, 0, 0}};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);

  EXPECT_EQ(callRegisterContractOf(T.Img, Tramp, Helper),
            CallRegisterContract::Platform);
  // A call into Go code, or one that names no target, stays Go's.
  EXPECT_EQ(callRegisterContractOf(T.Img, Tramp, Tramp),
            CallRegisterContract::GoCode);
  EXPECT_EQ(callRegisterContractOf(T.Img, Tramp, InvalidVA),
            CallRegisterContract::GoCode);

  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  llvm::LLVMContext Context;
  const PipelineResult R = Pipeline().run(T.Img, Context, Options);
  ASSERT_TRUE(R.Success) << R.Error;
  EXPECT_EQ(R.MedIRVerifierFailures, 0u);
  const std::string Text = highCBody(R, T.Img, "runtime_tramp");
  ASSERT_FALSE(Text.empty());
  EXPECT_EQ(Text.find("__builtin_trap"), std::string::npos) << Text;
  EXPECT_NE(Text.find("helper("), std::string::npos) << Text;

  // Without the table of the functions the pclntab records, nothing tells
  // code it left out from code inside a function: the call stays Go's.
  T.Img.ExceptionMetadata.GoModule->FrameArguments.clear();
  EXPECT_EQ(callRegisterContractOf(T.Img, Tramp, Helper),
            CallRegisterContract::GoCode);
}

TEST(GoStackABI, ARoutineThatJumpsToPlatformCodeHandsOnItsRegisters) {
  // Go 1.16's syscall.libc_mmap_trampoline is `B libc_mmap(SB)`: the libc
  // routine takes its arguments in the platform's registers, which the
  // trampoline was entered with and hands on.  It reads none of them, so
  // as stack-ABI code it took no register argument and passed six unknown
  // values.
  //   syscall.libc_add_trampoline: b helper
  //   helper:                      add x0, x0, x1; ret
  constexpr va_t Tramp = kTextVA + 0x100;
  constexpr va_t Helper = kTextVA + 0x200;
  GoTestImage T(Arch::AArch64);
  T.writeText(Tramp,
              words({0x14000000u | (((Helper - Tramp) >> 2) & 0x3FFFFFFu)}));
  T.writeText(Helper, words({0x8B010000, 0xD65F03C0}));
  for (const auto &[Entry, Name] :
       {std::pair{Tramp, "syscall.libc_add_trampoline"},
        std::pair{Helper, "helper"}}) {
    Symbol Function = Symbol::makeFunc(Entry);
    Function.Name = Name;
    T.Img.Symbols.push_back(std::move(Function));
  }
  GoModuleInfo Module;
  Module.StackArguments = true;
  Module.GoCode = {{Tramp, Tramp + 0x10}};
  Module.FrameArguments = {{Tramp, 0}};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);

  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  llvm::LLVMContext Context;
  const PipelineResult R = Pipeline().run(T.Img, Context, Options);
  ASSERT_TRUE(R.Success) << R.Error;
  EXPECT_EQ(R.MedIRVerifierFailures, 0u);
  const auto It = llvm::find_if(
      R.MedFuncs, [&](const MedFunc &M) { return M.Entry == Tramp; });
  ASSERT_NE(It, R.MedFuncs.end());
  EXPECT_FALSE(It->StackArgumentsOnly);
  const std::string Text = highCBody(R, T.Img, "syscall_libc_add_trampoline");
  ASSERT_FALSE(Text.empty());
  EXPECT_EQ(Text.find("__builtin_trap"), std::string::npos) << Text;
  EXPECT_NE(Text.find("helper(arg0, arg1)"), std::string::npos) << Text;
}

TEST(GoAssembly, ACallThroughAPointerThatIsNoFuncValueFollowsThePlatform) {
  // Assembly calls Go code by name or through a func value, whose code
  // pointer is the word the closure context register points at.  A call
  // through any other pointer it loads reaches the platform's code:
  // runtime.nanotime1 calls the vDSO routine a variable holds, with its
  // stack pointer in R20, which that routine keeps and Go code would not.
  //   plat:  stp x29, x30, [sp, #-32]!; mov x20, x0; ldr x2, [x1]; blr x2
  //          str x0, [x20]; ldp x29, x30, [sp], #32; ret
  //   fval:  the same, with `mov x26, x1` before the load
  //   cross: the same as plat, with `cbz x2` between the load and the call
  //   join:  the same as plat, with `cbz x3, 1f; add x4, x4, #1; 1:` there,
  //          so two blocks lead to the call's
  constexpr va_t Plat = kTextVA + 0x100;
  constexpr va_t FuncValue = kTextVA + 0x140;
  constexpr va_t Cross = kTextVA + 0x180;
  constexpr va_t Joined = kTextVA + 0x1C0;
  GoTestImage T(Arch::AArch64);
  T.writeText(Plat, words({0xA9BE7BFD, 0xAA0003F4, 0xF9400022, 0xD63F0040,
                           0xF9000280, 0xA8C27BFD, 0xD65F03C0}));
  T.writeText(Joined,
              words({0xA9BE7BFD, 0xAA0003F4, 0xF9400022, 0xB4000043, 0x91000484,
                     0xD63F0040, 0xF9000280, 0xA8C27BFD, 0xD65F03C0}));
  T.writeText(FuncValue,
              words({0xA9BE7BFD, 0xAA0003F4, 0xAA0103FA, 0xF9400022, 0xD63F0040,
                     0xF9000280, 0xA8C27BFD, 0xD65F03C0}));
  T.writeText(Cross, words({0xA9BE7BFD, 0xAA0003F4, 0xF9400022, 0xB4000062,
                            0xD63F0040, 0xF9000280, 0xA8C27BFD, 0xD65F03C0}));
  for (const auto &[Entry, Name] :
       {std::pair{Plat, "runtime.plat"}, std::pair{FuncValue, "runtime.fval"},
        std::pair{Cross, "runtime.cross"}, std::pair{Joined, "runtime.join"}}) {
    Symbol Function = Symbol::makeFunc(Entry);
    Function.Name = Name;
    T.Img.Symbols.push_back(std::move(Function));
  }
  GoModuleInfo Module;
  Module.PclnTabMagic = 0xFFFFFFF1u;
  Module.RegisterArguments = true;
  Module.GoCode = {{kTextVA, kTextVA + 0x400}};
  Module.GoAssembly = {{Plat, Plat + 0x100}};
  Module.FrameArguments = {
      {Plat, 0, 0}, {FuncValue, 0, 0}, {Cross, 0, 0}, {Joined, 0, 0}};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);

  // The contract of the routine's one indirect call.
  const auto Contract = [&](va_t Entry) {
    const MedFunc F = lift(T.Img, Entry);
    for (const MedBlock &Block : F.Blocks)
      for (const MedOp &Op : Block.Ops)
        if (Op.Opcode == NdOp::INDIR_CALL)
          return Op.RegisterContract;
    ADD_FAILURE() << "no indirect call";
    return CallRegisterContract::Platform;
  };
  EXPECT_EQ(Contract(Plat), CallRegisterContract::Platform);
  EXPECT_EQ(Contract(Cross), CallRegisterContract::Platform);
  EXPECT_EQ(Contract(Joined), CallRegisterContract::Platform);
  EXPECT_EQ(Contract(FuncValue), CallRegisterContract::GoCode);
  // Compiled code calls through method tables and func values alike: only
  // assembly's pointers are told apart.
  T.Img.ExceptionMetadata.GoModule->GoAssembly.clear();
  EXPECT_EQ(Contract(Plat), CallRegisterContract::GoInternal);
  T.Img.ExceptionMetadata.GoModule->GoAssembly = {{Plat, Plat + 0x100}};

  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  llvm::LLVMContext Context;
  const PipelineResult R = Pipeline().run(T.Img, Context, Options);
  ASSERT_TRUE(R.Success) << R.Error;
  EXPECT_EQ(R.MedIRVerifierFailures, 0u);
  for (const char *Name : {"runtime_plat", "runtime_cross", "runtime_join"}) {
    const std::string Text = highCBody(R, T.Img, Name);
    ASSERT_FALSE(Text.empty()) << Name;
    EXPECT_EQ(Text.find("__builtin_trap"), std::string::npos) << Text;
  }
  // After a call into Go code X20 holds nothing.
  const std::string Go = highCBody(R, T.Img, "runtime_fval");
  ASSERT_FALSE(Go.empty());
  EXPECT_NE(Go.find("__builtin_trap"), std::string::npos) << Go;
}

TEST(GoAssembly, AssemblyEnteredAsAClosureReadsItsContext) {
  // The compiler's closures for comparing values of a size they hold jump
  // to runtime.memequal_varlen, internal-ABI assembly that reads the size
  // through the closure context.  The context was defined for compiled
  // code alone, so the read went through a register nothing defines.
  //   main.eq: mov rcx, [rdx+8]; add rax, rbx; add rax, rcx; ret
  //   (arm64)  ldr x2, [x26, #8]; add x0, x0, x1; add x0, x0, x2; ret
  for (const Arch A : {Arch::X64, Arch::AArch64}) {
    SCOPED_TRACE(static_cast<int>(A));
    constexpr va_t F = kTextVA + 0x100;
    GoTestImage T(A);
    if (A == Arch::X64)
      T.writeText(F, {0x48, 0x8B, 0x4A, 0x08, 0x48, 0x01, 0xD8, 0x48, 0x01,
                      0xC8, 0xC3});
    else
      T.writeText(F, words({0xF9400742, 0x8B010000, 0x8B020000, 0xD65F03C0}));
    Symbol Function = Symbol::makeFunc(F);
    Function.Name = "main.eq";
    T.Img.Symbols.push_back(std::move(Function));
    GoModuleInfo Module;
    Module.PclnTabMagic = 0xFFFFFFF1u;
    Module.RegisterArguments = true;
    Module.GoCode = {{kTextVA, kTextVA + 0x400}};
    Module.GoAssembly = {{F, F + 0x20}};
    Module.FrameArguments = {{F, 0, 0}};
    T.Img.ExceptionMetadata.GoModule = std::move(Module);
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    llvm::LLVMContext Context;
    const PipelineResult R = Pipeline().run(T.Img, Context, Options);
    ASSERT_TRUE(R.Success) << R.Error;
    EXPECT_EQ(R.MedIRVerifierFailures, 0u);
    const std::string Text = highCBody(R, T.Img, "main_eq");
    ASSERT_FALSE(Text.empty());
    EXPECT_NE(Text.find(A == Arch::X64 ? "movq %%rdx" : "mov %0, x26"),
              std::string::npos)
        << Text;
    EXPECT_EQ(Text.find("__builtin_trap"), std::string::npos) << Text;
  }
}

enum class JoinedClosureOperand { Context, LoadAddress, AgreedContext };

void expectJoinedClosure(JoinedClosureOperand Operand) {
  constexpr va_t Caller = kTextVA + 0x100;
  constexpr va_t Callee = kTextVA + 0x200;
  GoTestImage T(Arch::AArch64);

  // Callers enter with x1/x4 pointing at the two descriptors in the image.
  // Both describe this same closure body. Only the captured word differs.
  T.writeText(Callee, words({0xF9400740, 0xD65F03C0})); // ldr x0,[x26,#8]; ret
  const auto descriptor = [&](uint32_t Capture) {
    return T.addPayload(
        words({uint32_t(Callee), uint32_t(Callee >> 32), Capture, 0}));
  };
  const va_t First = descriptor(7), Second = descriptor(41);
  ASSERT_NE(First, Second);
  const bool ChangeAddress = Operand == JoinedClosureOperand::LoadAddress;
  const bool ChangeContext = Operand == JoinedClosureOperand::Context;
  const uint32_t BeforeBranch = ChangeAddress ? 0xAA0103E6 : 0xF9400022;
  const uint32_t OnFallthrough = ChangeAddress   ? 0xAA0403E6
                                 : ChangeContext ? 0xAA0403FA
                                                 : 0xAA0103FA;
  // Save LR/frame. Set x26=x1. Preserve the shared call target when x26
  // changes, or load the shared target through the merged x6 when x26 agrees.
  // The two arms are selected by an independent x3, not by the call target.
  T.writeText(Caller,
              words({0xA9BE7BFD, 0xAA0103FA, BeforeBranch, 0xB4000043,
                     OnFallthrough, ChangeAddress ? 0xF94000C2u : 0xD503201Fu,
                     0xD63F0040, 0xA8C27BFD, 0xD65F03C0}));
  for (const auto &[Entry, Name] :
       {std::pair{Caller, "runtime.joinedClosure"},
        std::pair{Callee, "main.makeClosure.func1"}}) {
    Symbol Function = Symbol::makeFunc(Entry);
    Function.Name = Name;
    T.Img.Symbols.push_back(std::move(Function));
  }
  GoModuleInfo Module;
  Module.PclnTabMagic = 0xFFFFFFF1u;
  Module.RegisterArguments = true;
  Module.GoCode = {{kTextVA, kTextVA + 0x400}};
  Module.GoAssembly = {{Caller, Caller + 0x24}};
  Module.FrameArguments = {{Caller, 0, 0}, {Callee, 0, 0}};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);

  const MedFunc F = lift(T.Img, Caller);
  const MedOp *Call = nullptr;
  for (const MedBlock &Block : F.Blocks)
    for (const MedOp &Op : Block.Ops)
      if (Op.Opcode == NdOp::INDIR_CALL) {
        ASSERT_EQ(Call, nullptr);
        ASSERT_EQ(Block.Preds.size(), 2u);
        ASSERT_NE(Block.Preds[0], Block.Preds[1]);
        Call = &Op;
      }
  ASSERT_NE(Call, nullptr);
  ASSERT_EQ(Call->Addr, Caller + 24);
  EXPECT_EQ(Call->RegisterContract, CallRegisterContract::GoCode);
  const TargetRegInfo &TRI = getTargetRegInfo(Arch::AArch64);
  EXPECT_TRUE(callClobbersRegister(*Call, a64reg::X20, 8, TRI, T.Img.Format));
  EXPECT_FALSE(
      callClobbersRegister(*Call, TRI.StackPointer, 8, TRI, T.Img.Format));
  EXPECT_FALSE(
      callClobbersRegister(*Call, TRI.FramePointer, 8, TRI, T.Img.Format));
}

TEST(GoAssembly, MixedIncomingClosureContextIsNotProofOfAPlatformCall) {
  expectJoinedClosure(JoinedClosureOperand::Context);
}

TEST(GoAssembly, MixedIncomingLoadAddressIsNotProofOfAPlatformCall) {
  expectJoinedClosure(JoinedClosureOperand::LoadAddress);
}

TEST(GoAssembly, AgreedIncomingClosureContextRetainsTheGoContract) {
  expectJoinedClosure(JoinedClosureOperand::AgreedContext);
}

TEST(GoAssembly, ACallOnOneIncomingEdgeCannotProveAPlatformCall) {
  constexpr va_t Caller = kTextVA + 0x100;
  constexpr va_t Helper = Caller + 0x80;
  constexpr va_t Closure = kTextVA + 0x200;
  GoTestImage T(Arch::AArch64);
  // Incoming x1 points at the descriptor below. Save it in a private stack
  // slot. The helper really overwrites x26; the caller restores it only on
  // that edge. Without the helper, the original x26 reaches the join.
  // Both edges therefore pass the same valid descriptor to the same closure.
  T.writeText(Caller, words({
                          0xA9BE7BFD, // stp x29, x30, [sp, #-32]!
                          0xAA0103FA, // mov x26, x1
                          0xF9000BE1, // str x1, [sp, #16]
                          0xB4000063, // cbz x3, join
                          0x9400001C, // bl Helper
                          0xF9400BFA, // ldr x26, [sp, #16]
                          0xF9400BE6, // join: ldr x6, [sp, #16]
                          0xF94000C2, // ldr x2, [x6]
                          0xD63F0040, // blr x2
                          0xA8C27BFD, // ldp x29, x30, [sp], #32
                          0xD65F03C0  // ret
                      }));
  T.writeText(Helper, words({0xAA1F03FA, 0xD65F03C0}));  // mov x26,xzr; ret
  T.writeText(Closure, words({0xF9400740, 0xD65F03C0})); // ldr x0,[x26,#8]; ret
  const va_t Descriptor =
      T.addPayload(words({uint32_t(Closure), uint32_t(Closure >> 32), 63, 0}));
  ASSERT_NE(Descriptor, 0u);
  for (const auto &[Entry, Name] :
       {std::pair{Caller, "runtime.joinedClosureAfterCall"},
        std::pair{Helper, "runtime.clearContext"},
        std::pair{Closure, "main.makeClosure.func1"}}) {
    Symbol Function = Symbol::makeFunc(Entry);
    Function.Name = Name;
    T.Img.Symbols.push_back(std::move(Function));
  }
  GoModuleInfo Module;
  Module.PclnTabMagic = 0xFFFFFFF1u;
  Module.RegisterArguments = true;
  Module.GoCode = {{kTextVA, kTextVA + 0x400}};
  Module.GoAssembly = {{Caller, Caller + 44}, {Helper, Helper + 8}};
  Module.FrameArguments = {{Caller, 0, 0}, {Helper, 0, 0}, {Closure, 0, 0}};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);

  const LowFunc Low = build(T.Img, Caller);
  const LowBlock *CallBlock = nullptr;
  const LowBlock *JoinBlock = nullptr;
  for (const LowBlock &Block : Low.Blocks)
    for (const LowOp &Op : Block.Ops) {
      if (Op.Opcode == NdOp::CALL) {
        ASSERT_EQ(CallBlock, nullptr);
        ASSERT_EQ(Op.Addr, Caller + 16);
        CallBlock = &Block;
      }
      if (Op.Opcode == NdOp::INDIR_CALL) {
        ASSERT_EQ(JoinBlock, nullptr);
        ASSERT_EQ(Op.Addr, Caller + 32);
        JoinBlock = &Block;
      }
    }
  ASSERT_NE(CallBlock, nullptr);
  ASSERT_NE(JoinBlock, nullptr);
  ASSERT_EQ(JoinBlock->Preds.size(), 2u);
  EXPECT_TRUE(llvm::is_contained(JoinBlock->Preds, CallBlock->Id));
  for (const va_t Address : {Caller + 24, Caller + 28})
    EXPECT_TRUE(llvm::any_of(JoinBlock->Ops, [&](const LowOp &Op) {
      return Op.Addr == Address && Op.Opcode == NdOp::LOAD;
    })) << Address;

  LowToMedConverter Converter;
  Converter.setBinaryImage(&T.Img);
  const MedFunc F = Converter.convert(Low, T.Img.Arch, T.Img.Format);
  const MedOp *Call = nullptr;
  for (const MedBlock &Block : F.Blocks)
    for (const MedOp &Op : Block.Ops)
      if (Op.Opcode == NdOp::INDIR_CALL) {
        ASSERT_EQ(Call, nullptr);
        Call = &Op;
      }
  ASSERT_NE(Call, nullptr);
  ASSERT_EQ(Call->Addr, Caller + 32);
  EXPECT_EQ(Call->RegisterContract, CallRegisterContract::GoCode);
  const TargetRegInfo &TRI = getTargetRegInfo(Arch::AArch64);
  EXPECT_TRUE(callClobbersRegister(*Call, a64reg::X20, 8, TRI, T.Img.Format));
  EXPECT_FALSE(
      callClobbersRegister(*Call, TRI.StackPointer, 8, TRI, T.Img.Format));
  EXPECT_FALSE(
      callClobbersRegister(*Call, TRI.FramePointer, 8, TRI, T.Img.Format));
}

} // namespace
