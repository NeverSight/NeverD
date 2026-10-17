//===- AAPCS64CallContractTests.cpp - AArch64 call arguments -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// AArch64 code reaches another module's routine through the pointer it loads
// from the slot the loader binds to that routine: a PLT entry's `adrp x16;
// ldr x17, [x16, #slot]; add x16, x16, #slot; br x17`, or a call built with
// -fno-plt.  The routine's prototype gives the arguments it takes, which a
// function passing its own on takes as its parameters.
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/loader/FunctionDiscovery.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace neverd;

constexpr va_t Text = 0x10000;
constexpr va_t Plt = 0x10100;
constexpr va_t Got = 0x20000;

uint32_t adrp(unsigned Rd, va_t At, va_t Target) {
  const uint32_t Pages =
      static_cast<uint32_t>((Target >> 12) - (At >> 12)) & 0x1FFFFF;
  return 0x90000000u | (Pages & 3) << 29 | (Pages >> 2) << 5 | Rd;
}
uint32_t ldrX(unsigned Rt, unsigned Rn, va_t Slot) {
  return 0xF9400000u | static_cast<uint32_t>(Slot & 0xFFF) / 8 << 10 | Rn << 5 |
         Rt;
}
uint32_t addX(unsigned Rd, unsigned Rn, va_t Slot) {
  return 0x91000000u | static_cast<uint32_t>(Slot & 0xFFF) << 10 | Rn << 5 | Rd;
}
uint32_t br(unsigned Rn) { return 0xD61F0000u | Rn << 5; }
uint32_t blr(unsigned Rn) { return 0xD63F0000u | Rn << 5; }
constexpr uint32_t PushFrame = 0xA9BF7BFDu; // stp x29, x30, [sp, #-16]!
constexpr uint32_t SetFrame = 0x910003FDu;  // mov x29, sp
constexpr uint32_t AddW0One = 0x11000400u;  // add w0, w0, #1
constexpr uint32_t PopFrame = 0xA8C17BFDu;  // ldp x29, x30, [sp], #16
constexpr uint32_t Ret = 0xD65F03C0u;

/// The canonical PLT entry at \p At for the import in \p Slot.
std::vector<uint32_t> pltEntry(va_t At, va_t Slot) {
  return {adrp(16, At, Slot), ldrX(17, 16, Slot), addX(16, 16, Slot), br(17)};
}

void place(Segment &Seg, va_t At, const std::vector<uint32_t> &Words) {
  for (size_t I = 0; I < Words.size(); ++I)
    for (unsigned B = 0; B < 4; ++B)
      Seg.Data[At - Seg.VA + I * 4 + B] =
          static_cast<uint8_t>(Words[I] >> (8 * B));
}

Segment codeSegment(const char *Name, va_t VA, size_t Size) {
  Segment Seg;
  Seg.Name = Name;
  Seg.VA = VA;
  Seg.Size = Seg.FileSz = Size;
  Seg.Data.assign(Size, 0);
  Seg.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  return Seg;
}

/// An AArch64 ELF image with \p Code placed at its addresses, the function
/// symbols \p Functions, and \p Imports bound to consecutive GOT slots from
/// Got.  The loader binds the slot of a symbol no module of the image
/// defines; one the image defines (\p Defined) it relocates to the image's
/// own routine.  The PLT entries pair with their imports as the loader pairs
/// them.
BinaryImage
makeImage(const std::vector<std::pair<va_t, std::vector<uint32_t>>> &Code,
          const std::vector<std::pair<va_t, const char *>> &Functions,
          const std::vector<const char *> &Imports, bool Defined = false) {
  BinaryImage Img;
  Img.Arch = Arch::AArch64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::ELF;
  Segment TextSeg = codeSegment(".text", Text, Plt - Text);
  Segment PltSeg = codeSegment(".plt", Plt, 0x100);
  for (const auto &[At, Words] : Code)
    place(At < Plt ? TextSeg : PltSeg, At, Words);
  Img.Segments.push_back(std::move(TextSeg));
  Img.Segments.push_back(std::move(PltSeg));
  Segment GotSeg;
  GotSeg.Name = ".got";
  GotSeg.VA = Got;
  GotSeg.Size = GotSeg.FileSz = 8 * Imports.size();
  GotSeg.Data.assign(GotSeg.Size, 0);
  GotSeg.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Img.Segments.push_back(std::move(GotSeg));
  for (size_t I = 0; I < Imports.size(); ++I) {
    Img.Imports.push_back({"libc.so", Imports[I], 0, Got + 8 * I});
    if (!Defined)
      EXPECT_TRUE(Img.recordImportStorageSlot(
          Got + 8 * I, Imports[I], 0, ImportStorageEvidence::LoaderBind));
  }
  for (const auto &[Entry, Name] : Functions) {
    Symbol Function = Symbol::makeFunc(Entry);
    Function.Name = Name;
    Img.Symbols.push_back(std::move(Function));
  }
  scanImportThunks(Img);
  return Img;
}

PipelineResult run(const BinaryImage &Img, llvm::LLVMContext &Ctx,
                   std::set<va_t> Entries, bool LLVM = false) {
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries = std::move(Entries);
  Opts.LiftMode = LLVM;
  return Pipeline().run(Img, Ctx, Opts);
}

/// HighC for the functions at \p Entries of \p Img.
std::string highC(const BinaryImage &Img, std::set<va_t> Entries) {
  llvm::LLVMContext Ctx;
  const PipelineResult Result = run(Img, Ctx, std::move(Entries));
  EXPECT_TRUE(Result.Success) << Result.Error;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Img.Arch;
  Options.Format = Img.Format;
  Options.Image = &Img;
  EXPECT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, Options));
  return Source;
}

/// The text of function \p Name's definition, its signature included.
std::string body(const std::string &Source, const std::string &Name) {
  const size_t At = Source.find(" " + Name + "(");
  if (At == std::string::npos)
    return {};
  const size_t Open = Source.find('{', At);
  const size_t Close = Source.find("\n}", Open);
  if (Open == std::string::npos || Close == std::string::npos)
    return {};
  const size_t Begin = Source.rfind('\n', At) + 1;
  return Source.substr(Begin, Close + 2 - Begin);
}

TEST(AAPCS64CallContract, APLTEntryPassesTheArgumentsOfItsImport) {
  // islower takes one argument, which its PLT entry passes on in x0 without
  // writing it; getuid takes none.
  constexpr va_t Lower = Plt, Uid = Plt + 0x10;
  const BinaryImage Img =
      makeImage({{Lower, pltEntry(Lower, Got)}, {Uid, pltEntry(Uid, Got + 8)}},
                {}, {"islower", "getuid"});
  ASSERT_NE(Img.findImportAt(Lower), nullptr);
  const std::string Source = highC(Img, {Lower, Uid});
  const std::string LowerBody = body(Source, "sub_10100");
  ASSERT_FALSE(LowerBody.empty()) << Source;
  EXPECT_NE(LowerBody.find("sub_10100(int64_t arg0)"), std::string::npos)
      << LowerBody;
  EXPECT_NE(LowerBody.find("islower(arg0)"), std::string::npos) << LowerBody;
  EXPECT_EQ(LowerBody.find("unknown value"), std::string::npos) << LowerBody;
  const std::string UidBody = body(Source, "sub_10110");
  ASSERT_FALSE(UidBody.empty()) << Source;
  EXPECT_NE(UidBody.find("sub_10110(void)"), std::string::npos) << UidBody;
  EXPECT_NE(UidBody.find("getuid()"), std::string::npos) << UidBody;
}

/// lower(c) tail-calls islower and bump(c) returns toupper(c) + 1, each
/// through the GOT slot the call loads, as -fno-plt builds them.
BinaryImage makeGOTCalls() {
  constexpr va_t Lower = Text, Bump = Text + 0x10;
  constexpr va_t LowerSlot = Got, BumpSlot = Got + 8;
  return makeImage(
      {{Lower, {adrp(1, Lower, LowerSlot), ldrX(1, 1, LowerSlot), br(1)}},
       {Bump,
        {PushFrame, SetFrame, adrp(8, Bump + 8, BumpSlot), ldrX(8, 8, BumpSlot),
         blr(8), AddW0One, PopFrame, Ret}}},
      {{Lower, "lower"}, {Bump, "bump"}}, {"islower", "toupper"});
}

TEST(AAPCS64CallContract, ACallThroughAGOTSlotPassesTheIncomingArgument) {
  const BinaryImage Img = makeGOTCalls();
  const std::string Source = highC(Img, {Text, Text + 0x10});
  const std::string Lower = body(Source, "lower");
  ASSERT_FALSE(Lower.empty()) << Source;
  EXPECT_NE(Lower.find("lower(int64_t arg0)"), std::string::npos) << Lower;
  EXPECT_NE(Lower.find("islower(arg0)"), std::string::npos) << Lower;
  const std::string Bump = body(Source, "bump");
  ASSERT_FALSE(Bump.empty()) << Source;
  EXPECT_NE(Bump.find("toupper(arg0)"), std::string::npos) << Bump;
  EXPECT_EQ(Source.find("unknown value"), std::string::npos) << Source;
}

TEST(AAPCS64CallContract, TheLLVMRoutePassesTheArgumentThroughAGOTSlot) {
  // The call through the slot passes the function's own argument, whether
  // the emitter calls the import or the word it loads.
  const BinaryImage Img = makeGOTCalls();
  llvm::LLVMContext Ctx;
  const PipelineResult Result = run(Img, Ctx, {Text, Text + 0x10}, true);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_TRUE(Result.LlvmModule);
  for (const char *Name : {"lower", "bump"}) {
    SCOPED_TRACE(Name);
    const llvm::Function *Fn = Result.LlvmModule->getFunction(Name);
    ASSERT_NE(Fn, nullptr);
    std::string IR;
    llvm::raw_string_ostream OS(IR);
    Fn->print(OS);
    ASSERT_EQ(Fn->arg_size(), 1u) << IR;
    const llvm::CallInst *Call = nullptr;
    for (const llvm::BasicBlock &Block : *Fn)
      for (const llvm::Instruction &I : Block)
        if (const auto *C = llvm::dyn_cast<llvm::CallInst>(&I);
            C && !llvm::isa<llvm::IntrinsicInst>(C))
          Call = C;
    ASSERT_NE(Call, nullptr) << IR;
    ASSERT_EQ(Call->arg_size(), 1u) << IR;
    EXPECT_EQ(Call->getArgOperand(0)->stripPointerCasts(), Fn->getArg(0)) << IR;
  }
}

TEST(AAPCS64CallContract,
     ASlotTheLoaderStartsAtThisImagesRoutinePassesItsArgs) {
  // A shared object's PLT entry for g, a function it defines itself: the
  // loader binds g's slot to g here, whose one argument the entry passes on.
  constexpr va_t G = Text, Stub = Plt;
  constexpr uint32_t AddW0W0 = 0x0B000000u; // add w0, w0, w0
  BinaryImage Img =
      makeImage({{G, {AddW0W0, Ret}}, {Stub, pltEntry(Stub, Got)}}, {{G, "g"}},
                {"g"}, /*Defined=*/true);
  for (unsigned B = 0; B < 8; ++B)
    Img.Segments.back().Data[B] = static_cast<uint8_t>(G >> (8 * B));
  Img.CodePtrRelocSlots.insert(Got);
  const std::string Source = highC(Img, {G, Stub});
  const std::string Entry = body(Source, "sub_10100");
  ASSERT_FALSE(Entry.empty()) << Source;
  EXPECT_NE(Entry.find("sub_10100(int64_t arg0)"), std::string::npos) << Entry;
  EXPECT_NE(Entry.find("(arg0)"), std::string::npos) << Entry;
  EXPECT_EQ(Entry.find("unknown value"), std::string::npos) << Entry;
}

} // namespace
