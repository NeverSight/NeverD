//===- I386CallContractTests.cpp - i386 stack-passed call arguments -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// i386 passes every argument on the stack.  MSVC pushes them; GCC stores
// each at [esp+N] below one adjustment.  Stores to the function's own locals
// in between are no arguments.  An import thunk `jmp dword ptr [slot]`
// reaches its import and passes on the arguments its caller left.  Only a
// Windows import may take ECX and EDX (__fastcall); an ELF import never
// takes a register argument.
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace neverd;

constexpr va_t Text = 0x401000;
constexpr va_t Iat = 0x403000;

/// The slot of import \p Index.
constexpr va_t slot(size_t Index) { return Iat + 4 * Index; }

/// `call dword ptr [Slot]`.
std::vector<uint8_t> callThroughSlot(va_t Slot) {
  return {0xFF,
          0x15,
          static_cast<uint8_t>(Slot),
          static_cast<uint8_t>(Slot >> 8),
          static_cast<uint8_t>(Slot >> 16),
          static_cast<uint8_t>(Slot >> 24)};
}

/// A PE32 image with \p Code at Text, the function `_wrap` there, and the
/// msvcrt.dll imports \p Imports in consecutive slots at Iat.
BinaryImage makeImage(std::vector<uint8_t> Code,
                      const std::vector<const char *> &Imports) {
  BinaryImage Img;
  Img.Arch = Arch::X86;
  Img.Bits = Bitness::Bits32;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x400000;
  Img.Entry = Text;
  Segment Seg;
  Seg.Name = ".text";
  Seg.VA = Text;
  Seg.Size = Code.size();
  Seg.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Seg.Data = std::move(Code);
  Img.Segments.push_back(std::move(Seg));
  Segment Idata;
  Idata.Name = ".idata";
  Idata.VA = Iat;
  Idata.Size = Idata.FileSz = 4 * Imports.size();
  Idata.Data.resize(Idata.Size);
  Idata.Flags = SegmentFlags::Readable;
  Img.Segments.push_back(std::move(Idata));
  for (size_t I = 0; I < Imports.size(); ++I) {
    Import Imp;
    Imp.Module = "msvcrt.dll";
    Imp.Name = Imports[I];
    Imp.IATAddr = slot(I);
    Img.Imports.push_back(std::move(Imp));
  }
  Symbol Function = Symbol::makeFunc(Text);
  Function.Name = "_wrap";
  Img.Symbols.push_back(std::move(Function));
  return Img;
}

/// The call of \p Callee in the HighC of `wrap` in \p Img: the rest of its
/// line from the callee's name.
std::string callIn(const BinaryImage &Img, const std::string &Callee) {
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries = {Text};
  const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
  EXPECT_TRUE(Result.Success) << Result.Error;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Img.Arch;
  Options.Format = Img.Format;
  Options.Image = &Img;
  EXPECT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, Options));
  const size_t Body = Source.find(" wrap(");
  const size_t At = Source.find(Callee + "(", Body);
  if (Body == std::string::npos || At == std::string::npos) {
    ADD_FAILURE() << Source;
    return {};
  }
  return Source.substr(At, Source.find('\n', At) - At);
}

TEST(I386CallContract, AThunkJumpingThroughTheSlotForwardsItsArguments) {
  // `jmp dword ptr [__imp__calloc]`: the thunk calls calloc, not the value
  // it would load from an address, with the two arguments its caller left
  // on the stack.
  std::vector<uint8_t> Code = {0xFF,
                               0x25, // jmp dword ptr [slot]
                               static_cast<uint8_t>(slot(0)),
                               static_cast<uint8_t>(slot(0) >> 8),
                               static_cast<uint8_t>(slot(0) >> 16),
                               static_cast<uint8_t>(slot(0) >> 24)};
  const std::string Call = callIn(makeImage(Code, {"calloc"}), "calloc");
  EXPECT_NE(Call.find("arg0"), std::string::npos) << Call;
  EXPECT_NE(Call.find("arg1"), std::string::npos) << Call;
  EXPECT_EQ(Call.find("unknown value"), std::string::npos) << Call;
}

TEST(I386CallContract, ArgumentsStoredBelowOneAdjustmentAreAllPassed) {
  // MinGW's __getmainargs wrapper: GCC stores its fifth parameter for the
  // call first, then zeroes a local, and computes the other four arguments
  // in more ops than a fixed window of them reaches.
  std::vector<uint8_t> Code = {
      0x55,                                     // push ebp
      0x89, 0xE5,                               // mov ebp, esp
      0x53,                                     // push ebx
      0x83, 0xEC, 0x34,                         // sub esp, 0x34
      0x8B, 0x45, 0x18,                         // mov eax, [ebp+0x18]
      0xC7, 0x45, 0xEC, 0xFF, 0xFF, 0xFF, 0xFF, // mov [ebp-0x14], -1
      0xC7, 0x45, 0xF0, 0x00, 0x00, 0x00, 0x00, // mov [ebp-0x10], 0
      0x89, 0x44, 0x24, 0x10,                   // mov [esp+0x10], eax
      0x8B, 0x45, 0x14,                         // mov eax, [ebp+0x14]
      0xC7, 0x45, 0xF4, 0x00, 0x00, 0x00, 0x00, // mov [ebp-0xc], 0
      0x89, 0x44, 0x24, 0x0C,                   // mov [esp+0xc], eax
      0x8D, 0x45, 0xF4,                         // lea eax, [ebp-0xc]
      0x89, 0x44, 0x24, 0x08,                   // mov [esp+8], eax
      0x8D, 0x45, 0xF0,                         // lea eax, [ebp-0x10]
      0x89, 0x44, 0x24, 0x04,                   // mov [esp+4], eax
      0x8D, 0x45, 0xEC,                         // lea eax, [ebp-0x14]
      0x89, 0x04, 0x24,                         // mov [esp], eax
  };
  for (uint8_t B : callThroughSlot(slot(0)))
    Code.push_back(B);
  for (uint8_t B : {0x8B, 0x45, 0xEC, // mov eax, [ebp-0x14]
                    0x8B, 0x5D, 0xFC, // mov ebx, [ebp-4]
                    0xC9,             // leave
                    0xC3})            // ret
    Code.push_back(B);
  const std::string Call =
      callIn(makeImage(Code, {"__getmainargs"}), "__getmainargs");
  EXPECT_NE(Call.find("arg3, "), std::string::npos) << Call;
  EXPECT_NE(Call.find("arg4)"), std::string::npos) << Call;
}

TEST(I386CallContract, AStoreToALocalBetweenPushesIsNoArgument) {
  // calloc(n, size) pushed from the parameters, with a local written
  // between the two pushes.
  std::vector<uint8_t> Code = {
      0x55,                                     // push ebp
      0x89, 0xE5,                               // mov ebp, esp
      0x83, 0xEC, 0x08,                         // sub esp, 8
      0xFF, 0x75, 0x0C,                         // push [ebp+0xc]
      0xC7, 0x45, 0xF8, 0x07, 0x00, 0x00, 0x00, // mov [ebp-8], 7
      0xFF, 0x75, 0x08,                         // push [ebp+8]
  };
  for (uint8_t B : callThroughSlot(slot(0)))
    Code.push_back(B);
  for (uint8_t B : {0x83, 0xC4, 0x08, // add esp, 8
                    0xC9,             // leave
                    0xC3})            // ret
    Code.push_back(B);
  const std::string Call = callIn(makeImage(Code, {"calloc"}), "calloc");
  EXPECT_NE(Call.find("(arg0, arg1)"), std::string::npos) << Call;
  EXPECT_EQ(Call.find('7'), std::string::npos) << Call;
}

TEST(I386CallContract, APoppedPushIsNoArgument) {
  // clang's get-PC `call $+5; pop ebx` pushes the return address and pops
  // it, and `push eax` reserves a slot: a regparm callee that takes ECX gets
  // neither as its argument.
  std::vector<uint8_t> Code = {0x53,                         // push ebx
                               0x50,                         // push eax
                               0xE8, 0x00, 0x00, 0x00, 0x00, // call $+5
                               0x5B,                         // pop ebx
                               0x8D, 0x4C, 0x49, 0x01, // lea ecx, [ecx+ecx*2+1]
                               0xE8, 0x08, 0x00, 0x00, 0x00, // call helper
                               0x83, 0xC4, 0x04,             // add esp, 4
                               0x5B,                         // pop ebx
                               0xC3,                         // ret
                               0xCC, 0xCC, 0xCC,             // padding
                               0x8D, 0x04, 0x09, // helper: lea eax, [ecx+ecx]
                               0xC3};            // ret
  BinaryImage Img = makeImage(Code, {});
  Symbol Helper = Symbol::makeFunc(Text + 0x19);
  Helper.Name = "_helper";
  Img.Symbols.push_back(std::move(Helper));
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries = {Text, Text + 0x19};
  const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
  ASSERT_TRUE(Result.Success) << Result.Error;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Img.Arch;
  Options.Format = Img.Format;
  Options.Image = &Img;
  ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, Options));
  const size_t Call = Source.find("helper(", Source.find(" wrap("));
  ASSERT_NE(Call, std::string::npos) << Source;
  const std::string Line = Source.substr(Call, Source.find('\n', Call) - Call);
  // 0x401007 is the address `call $+5` pushes.
  EXPECT_EQ(Line.find("0x401007"), std::string::npos) << Source;
  EXPECT_EQ(Line.find("4198407"), std::string::npos) << Source;
  EXPECT_NE(Line.find("arg0"), std::string::npos) << Source;
}

TEST(I386CallContract, SelectingTheCallerPreservesTheCalleeArgumentContract) {
  for (BinaryFormat Format :
       {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
    for (bool Regparm : {false, true})
      for (bool Forwarder : {false, true})
        for (bool Selected : {false, true})
          for (bool NoOpt : {false, true})
            for (bool LLVM : {false, true}) {
              SCOPED_TRACE(std::to_string(static_cast<int>(Format)) + "/" +
                           std::to_string(Regparm) + "/" +
                           std::to_string(Forwarder) + "/" +
                           std::to_string(Selected) + "/" +
                           std::to_string(NoOpt) + "/" + std::to_string(LLVM));
              // Both register and stack setup are present. The callee's
              // actual reads decide which two values the call passes. ECX
              // is scratch, despite being first in the default register order.
              std::vector<uint8_t> Code = {
                  0xb8, 7,    0,    0,   0, // mov eax,7
                  0xba, 11,   0,    0,   0, // mov edx,11
                  0xb9, 99,   0,    0,   0, // mov ecx,99
                  0x6a, 47,   0x6a, 31,     // push 47; push 31
                  0xeb, 0,                  // jmp call_block
                  0xe8, 0x26, 0,    0,   0, // call helper at +0x40
                  0x83, 0xc4, 8,    0xc3};  // add esp,8; ret
              Code.resize(0x40, 0xcc);
              if (Forwarder) {
                Code.insert(Code.end(), {0xe9, 0x1b, 0, 0, 0});
                Code.resize(0x60, 0xcc);
              }
              const std::vector<uint8_t> Leaf =
                  Regparm ? std::vector<uint8_t>{0x8d, 0x04, 0x10, 0xc3}
                          : std::vector<uint8_t>{0x8b, 0x44, 0x24, 4,   0x03,
                                                 0x44, 0x24, 8,    0xc3};
              Code.insert(Code.end(), Leaf.begin(), Leaf.end());
              BinaryImage Img = makeImage(std::move(Code), {});
              Img.Format = Format;
              for (va_t Entry : {Text + 0x40, Text + 0x60}) {
                if (Entry == Text + 0x60 && !Forwarder)
                  continue;
                Symbol S = Symbol::makeFunc(Entry);
                S.Name = Entry == Text + 0x40 ? "helper" : "leaf";
                Img.Symbols.push_back(std::move(S));
              }
              llvm::LLVMContext Ctx;
              PipelineOptions Opts;
              Opts.EmitDumpOutput = false;
              Opts.NoOpt = NoOpt;
              Opts.LiftMode = LLVM;
              Opts.SourceProjection = true;
              Opts.OnlyFunctionEntries = {Text};
              if (!Selected) {
                Opts.OnlyFunctionEntries.insert(Text + 0x40);
                if (Forwarder)
                  Opts.OnlyFunctionEntries.insert(Text + 0x60);
              }
              const PipelineResult R = Pipeline().run(Img, Ctx, Opts);
              ASSERT_TRUE(R.Success) << R.Error;
              ASSERT_EQ(R.MedFuncs.size(), Selected ? 1u : Forwarder ? 3u : 2u);
              const MedFunc *Caller = nullptr;
              for (const MedFunc &F : R.MedFuncs)
                if (F.Entry == Text)
                  Caller = &F;
              ASSERT_NE(Caller, nullptr);
              const MedCallInfo *Call = nullptr;
              for (const MedCallInfo &CI : Caller->CallInfos)
                if (CI.TargetAddr == Text + 0x40)
                  Call = &CI;
              ASSERT_NE(Call, nullptr);
              if (Selected || !LLVM)
                ASSERT_EQ(Call->Args.size(), 2u);
              else
                ASSERT_GE(Call->Args.size(), 2u);
              // Follow only representation copies of the two literal inputs;
              // this oracle does not recover register or stack arguments.
              auto literal = [&](MedVar V) -> std::optional<uint64_t> {
                for (unsigned Depth = 0; Depth != 16; ++Depth) {
                  if (V.isConst())
                    return V.ConstVal;
                  const MedOp *Def = nullptr;
                  for (const MedBlock &B : Caller->Blocks)
                    for (const MedOp &O : B.Ops)
                      if (O.Output.Kind == V.Kind && O.Output.Id == V.Id &&
                          O.Output.SSAVer == V.SSAVer && O.Output.Size &&
                          (O.Opcode == NdOp::COPY ||
                           O.Opcode == NdOp::INT_ZEXT))
                        Def = &O;
                  if (!Def || Def->NumInputs != 1)
                    return std::nullopt;
                  V = Def->Inputs[0];
                }
                return std::nullopt;
              };
              EXPECT_EQ(literal(Call->Args[0]), Regparm ? 7u : 31u);
              EXPECT_EQ(literal(Call->Args[1]), Regparm ? 11u : 47u);
              if (LLVM) {
                ASSERT_NE(R.LlvmModule, nullptr);
                EXPECT_FALSE(llvm::verifyModule(*R.LlvmModule, &llvm::errs()));
              }
            }
}

TEST(I386CallContract, StoresThroughACopyOfESPAreArgumentsByOffset) {
  // clang -O0 addresses the outgoing area through a copy of ESP and stores
  // in any order, and keeps a computed argument in a callee-saved register.
  std::vector<uint8_t> Code = {
      0x55,             // push ebp
      0x89, 0xE5,       // mov ebp, esp
      0x56,             // push esi
      0x83, 0xEC, 0x14, // sub esp, 0x14
      0x8B, 0x45, 0x08, // mov eax, [ebp+8]
      0x8B, 0x75, 0x0C, // mov esi, [ebp+0xc]
      0x89, 0xE2,       // mov edx, esp
      0x89, 0x02,       // mov [edx], eax
      0x89, 0x72, 0x04, // mov [edx+4], esi
  };
  for (uint8_t B : callThroughSlot(slot(0)))
    Code.push_back(B);
  for (uint8_t B : {0x83, 0xC4, 0x14, // add esp, 0x14
                    0x5E,             // pop esi
                    0x5D,             // pop ebp
                    0xC3})            // ret
    Code.push_back(B);
  const std::string Call = callIn(makeImage(Code, {"calloc"}), "calloc");
  EXPECT_NE(Call.find("(arg0, arg1)"), std::string::npos) << Call;
}

TEST(I386CallContract, AStoreWiderThanASlotFillsEachSlotItCovers) {
  // clang -O0 copies an eight-byte structure argument into the outgoing area
  // with one `movsd [esp], xmm0`: its two halves are the first two arguments.
  std::vector<uint8_t> Code = {
      0x55,                         // push ebp
      0x89, 0xE5,                   // mov ebp, esp
      0x83, 0xEC, 0x18,             // sub esp, 0x18
      0xF2, 0x0F, 0x10, 0x45, 0x08, // movsd xmm0, [ebp+8]
      0x89, 0xE0,                   // mov eax, esp
      0xF2, 0x0F, 0x11, 0x00,       // movsd [eax], xmm0
  };
  for (uint8_t B : callThroughSlot(slot(0)))
    Code.push_back(B);
  for (uint8_t B : {0x83, 0xC4, 0x18, // add esp, 0x18
                    0x5D,             // pop ebp
                    0xC3})            // ret
    Code.push_back(B);
  const std::string Call = callIn(makeImage(Code, {"calloc"}), "calloc");
  EXPECT_EQ(Call.find("unknown"), std::string::npos) << Call;
  EXPECT_NE(Call.find(">> 32"), std::string::npos) << Call;
}

/// The HighC signature of the function at Text of \p Img, decompiled alone.
std::string signatureOf(const BinaryImage &Img, const std::string &Name) {
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries = {Text};
  const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
  EXPECT_TRUE(Result.Success) << Result.Error;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Img.Arch;
  Options.Format = Img.Format;
  Options.Image = &Img;
  EXPECT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, Options));
  const size_t At = Source.find(" " + Name + "(");
  if (At == std::string::npos) {
    ADD_FAILURE() << Source;
    return {};
  }
  const size_t Line = Source.rfind('\n', At) + 1;
  return Source.substr(Line, Source.find('\n', At) - Line);
}

TEST(I386CallContract, ARuntimeDoubleWordRoutineReturnsThePairAlone) {
  // libgcc's __udivdi3 returns its quotient in EDX:EAX.  Decompiled alone no
  // caller reads EDX after it, and its name says it returns the pair.
  const std::vector<uint8_t> Code = {0x8B, 0x44, 0x24, 0x04, // mov eax, [esp+4]
                                     0x8B, 0x54, 0x24, 0x08, // mov edx, [esp+8]
                                     0xC3};                  // ret
  BinaryImage Img = makeImage(Code, {});
  Img.Symbols.front().Name = "___udivdi3";
  const std::string Signature = signatureOf(Img, "__udivdi3");
  EXPECT_NE(Signature.find("64_t __udivdi3("), std::string::npos) << Signature;
  // Without that name nothing shows the high half is a result.
  const std::string Wrap = signatureOf(makeImage(Code, {}), "wrap");
  EXPECT_EQ(Wrap.find("64_t wrap("), std::string::npos) << Wrap;
}

TEST(I386CallContract, AStubOfAVariadicImportPassesItsArgumentsOn) {
  // `jmp dword ptr [__imp__fprintf]` passes every argument on, the variadic
  // ones too.  C passes no `...` on, so the stub hands them to vfprintf, which
  // takes them as fprintf does.
  const std::vector<uint8_t> Code = {0xFF,
                                     0x25, // jmp dword ptr [slot]
                                     static_cast<uint8_t>(slot(0)),
                                     static_cast<uint8_t>(slot(0) >> 8),
                                     static_cast<uint8_t>(slot(0) >> 16),
                                     static_cast<uint8_t>(slot(0) >> 24)};
  const BinaryImage Img = makeImage(Code, {"fprintf"});
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries = {Text};
  const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
  ASSERT_TRUE(Result.Success) << Result.Error;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Img.Arch;
  Options.Format = Img.Format;
  Options.Image = &Img;
  ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, Options));
  EXPECT_NE(Source.find("wrap jumps to the import fprintf"), std::string::npos)
      << Source;
  EXPECT_NE(Source.find("#include <stdarg.h>"), std::string::npos) << Source;
  EXPECT_NE(Source.find("int wrap(FILE *arg0, const char *format, ...) {"),
            std::string::npos)
      << Source;
  EXPECT_NE(Source.find("vfprintf(arg0, format, arguments);"),
            std::string::npos)
      << Source;
}

} // namespace

namespace {

MedVar register32(uint64_t Offset, int Version) {
  MedVar V;
  V.Kind = MedVar::Reg;
  V.Id = static_cast<int>(Offset);
  V.SSAVer = Version;
  V.Size = 4;
  V.RegOff = Offset;
  V.TheArch = Arch::X86;
  return V;
}

MedVar temporary32(int Id) {
  MedVar V;
  V.Kind = MedVar::Temp;
  V.Id = Id;
  V.Size = 4;
  V.TheArch = Arch::X86;
  return V;
}

void copyInto(MedBlock &Block, MedVar Output, MedVar Input) {
  MedOp Op;
  Op.Opcode = NdOp::COPY;
  Op.Output = Output;
  Op.addInput(Input);
  Block.Ops.push_back(Op);
}

/// The call-site arguments of a caller that leaves EDX set in the block of a
/// call to the import `routine` at \p Slot, an import whose signature no
/// table knows, and ECX set in the block before it, with \p Format; and
/// whether the caller gained a parameter.
std::pair<std::vector<MedVar>, bool> importCallArguments(BinaryFormat Format,
                                                         bool SetECX) {
  constexpr va_t Slot = 0x5000;
  MedFunc Func;
  Func.Entry = 0x1000;
  Func.Name = "caller";
  Func.Blocks.resize(2);
  Func.Blocks[0].Id = 0;
  Func.Blocks[0].Succs = {1};
  Func.Blocks[1].Id = 1;
  Func.Blocks[1].Preds = {0};
  if (SetECX)
    copyInto(Func.Blocks[0], register32(x86reg::RCX, 1), temporary32(1));
  copyInto(Func.Blocks[1], register32(x86reg::RDX, 1), temporary32(2));
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Output = register32(x86reg::RAX, 1);
  Call.addInput(MedVar::makeConst(Slot, 4));
  Func.Blocks[1].Ops.push_back(Call);

  BinaryImage Img;
  Img.Arch = Arch::X86;
  Img.Bits = Bitness::Bits32;
  Img.Format = Format;
  Img.Imports.push_back({"libroutine", "routine", 0, Slot});
  const std::map<va_t, std::string> Names{{Slot, "routine"}};
  recoverCallAbi(Func, Arch::X86, Names, &Img);
  EXPECT_EQ(Func.CallInfos.size(), 1u);
  return {Func.CallInfos.empty() ? std::vector<MedVar>{}
                                 : Func.CallInfos[0].Args,
          !Func.Params.empty()};
}

} // namespace

// An ELF import takes every argument on the stack, whether or not its
// signature is known: the ECX and EDX its caller left are no arguments, and
// a live-in ECX below them is no parameter the caller passes on.
TEST(I386CallContract, AnELFImportTakesNoRegisterArgument) {
  const auto [Arguments, GainedParameter] =
      importCallArguments(BinaryFormat::ELF, /*SetECX=*/true);
  EXPECT_TRUE(Arguments.empty());
  const auto [Forwarded, Promoted] =
      importCallArguments(BinaryFormat::ELF, /*SetECX=*/false);
  EXPECT_TRUE(Forwarded.empty());
  EXPECT_FALSE(Promoted);
}

// A Windows import of unknown signature may be __fastcall, so the ECX and
// EDX set before it remain its arguments.
TEST(I386CallContract, AWindowsImportMayTakeECXAndEDX) {
  const auto [Arguments, GainedParameter] =
      importCallArguments(BinaryFormat::COFF, /*SetECX=*/true);
  ASSERT_EQ(Arguments.size(), 2u);
  EXPECT_EQ(Arguments[0], temporary32(1));
  EXPECT_EQ(Arguments[1], temporary32(2));
}
