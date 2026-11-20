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
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/MedCallLeftovers.h"
#include "neverd/ir/med/MedPrologueSaves.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/LLVMContext.h"

#include <cstring>
#include <map>
#include <set>
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

TEST(GoStackGrowth, AFloatSpillNamesNoModelledArgumentList) {
  // runtime.modf: cmp rsp, [r14+0x10]; jbe grow; ret
  //   grow: movsd [rsp+8], xmm0; call morestack_noctxt
  //         movsd xmm0, [rsp+8]; jmp runtime.modf
  // Floating arguments are not modelled, so the spills are no argument list.
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
  const std::set<va_t> StackCallees{Add};
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
      const std::set<va_t> StackCallees{Add};
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
    ASSERT_TRUE(Effect.GoStubArguments.has_value());
    EXPECT_EQ(*Effect.GoStubArguments, *Arguments);
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
                     .GoStubArguments);
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

} // namespace
