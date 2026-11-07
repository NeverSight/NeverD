//===- ARM64SEHLocalUnwindTests.cpp - ARM64 local unwinds ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Windows ARM64 `_local_unwind` exits: the saved-frame protocol under the
/// ARM64 convention, where the frame a local unwind names is the entry stack
/// pointer, the handlers start with the parent's callee-saved registers and
/// its frame pointer, and nothing fixes the x0 the unwind resumes with.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/SEHLocalUnwindProtocol.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

using namespace neverd;

constexpr va_t Entry = 0x140001000;
constexpr va_t Foo = Entry + 0x80;
constexpr va_t Veneer = Entry + 0x90;
constexpr va_t Funclet = Entry + 0xa0;
constexpr va_t Target = Entry + 0x48;
constexpr va_t ImportSlot = 0x140002000;
constexpr uint32_t Nop = 0xd503201f;
constexpr uint32_t Ret = 0xd65f03c0;

/// xcpt4's shapes as MSVC ARM64 /O0 compiles them: the prologue saves the
/// entry stack pointer (`add x16, sp, #frame; str x16, [...]`), and a return
/// out of the `__try` reloads it for `_local_unwind`, whose target writes w0
/// before the epilogue.
enum class Shape {
  /// The handler stores 99 on an unwind and 100 on a normal exit through the
  /// Counter pointer homed at x29 + 0x10, which its x1 locates; the normal
  /// path keeps its inline copy.
  Guarded,
  /// The same, but the target returns without writing w0.
  ResultLive,
  /// x19 holds the parent's frame, and the handler stores through it to a
  /// local, as /GS builds address their frames.
  X19Local,
  /// The same handler stores through x19 to the saved-frame slot instead.
  X19Slot,
};

std::vector<uint32_t> parentCode(Shape S) {
  switch (S) {
  case Shape::Guarded:
  case Shape::ResultLive:
    return {
        0xa9be7bfd,                              // stp x29, x30, [sp, #-0x20]!
        0x910003fd,                              // mov x29, sp
        0x910083f0,                              // add x16, sp, #0x20
        0xf9000fb0,                              // str x16, [x29, #0x18]
        0xf9000ba1,                              // str x1, [x29, #0x10]
        0x340000e0,                              // cbz w0, work
        0x90000008,                              // adrp x8, Entry
        0x91012101,                              // add x1, x8, #0x48
        0xf9400fa0,                              // ldr x0, [x29, #0x18]
        0x9400001b,                              // bl veneer
        Nop,                                     //
        0x14000007,                              // b done
        0x94000014,                              // work: bl foo
        Nop,                                     //
        0xf9400ba8,                              // ldr x8, [x29, #0x10]
        0x52800c89,                              // mov w9, #100
        0xb9000109,                              // str w9, [x8]
        Nop,                                     //
        S == Shape::Guarded ? 0x52800020u : Nop, // done: mov w0, #1
        0xa8c27bfd,                              // ldp x29, x30, [sp], #0x20
        Ret,
    };
  case Shape::X19Local:
  case Shape::X19Slot:
    return {
        0xa9bd7bfd, // stp x29, x30, [sp, #-0x30]!
        0xf9000bf3, // str x19, [sp, #0x10]
        0x910003fd, // mov x29, sp
        0x910003f3, // mov x19, sp
        0x9100c3f0, // add x16, sp, #0x30
        0xf9001670, // str x16, [x19, #0x28]
        0xb9001a7f, // str wzr, [x19, #0x18]
        0x340000e0, // cbz w0, work
        0x90000008, // adrp x8, Entry
        0x91012101, // add x1, x8, #0x48
        0xf9401660, // ldr x0, [x19, #0x28]
        0x94000019, // bl veneer
        Nop,        //
        0x14000005, // b done
        0x94000012, // work: bl foo
        Nop,        //
        0x52800c89, // mov w9, #100
        0xb9001a69, // str w9, [x19, #0x18]
        0xb9401a60, // done: ldr w0, [x19, #0x18]
        0xf9400bf3, // ldr x19, [sp, #0x10]
        0xa8c37bfd, // ldp x29, x30, [sp], #0x30
        Ret,
    };
  }
  return {};
}

std::vector<uint32_t> handlerCode(Shape S) {
  if (S == Shape::Guarded || S == Shape::ResultLive)
    return {
        0xa9bf7bfd, // stp x29, x30, [sp, #-0x10]!
        0xaa0103fd, // mov x29, x1
        0x52800c69, // mov w9, #99
        0x35000040, // cbnz w0, store
        0x52800c89, // mov w9, #100
        0xf9400ba8, // store: ldr x8, [x29, #0x10]
        0xb9000109, // str w9, [x8]
        0xa8c17bfd, // ldp x29, x30, [sp], #0x10
        Ret,
    };
  return {
      0xa9bf7bfd, // stp x29, x30, [sp, #-0x10]!
      0x52800c69, // mov w9, #99
      0x35000040, // cbnz w0, store
      0x52800c89, // mov w9, #100
      // store: str w9, [x19, #0x18] or str x9, [x19, #0x28]
      S == Shape::X19Local ? 0xb9001a69u : 0xf9001669u,
      0xa8c17bfd, // ldp x29, x30, [sp], #0x10
      Ret,
  };
}

UnwindOperation unwindOp(UnwindOperationKind Kind, uint64_t StackOffset = 0,
                         uint16_t Register = 0) {
  UnwindOperation Op;
  Op.Kind = Kind;
  Op.StackOffset = StackOffset;
  Op.Register = Register;
  Op.RegisterClass = UnwindRegisterClass::GeneralPurpose;
  return Op;
}

/// The parent at Entry, `foo` (a `ret`), the import veneer and the
/// termination handler.  \p RegisterStub records the veneer as the import's
/// stub, as whole-image discovery does; without it only its code says so.
BinaryImage makeImage(Shape S, bool RegisterStub = true) {
  BinaryImage Img;
  Img.Arch = Arch::AArch64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  Img.Entry = Entry;
  std::vector<uint32_t> Words(0xd0 / 4, Nop);
  const std::vector<uint32_t> Parent = parentCode(S);
  std::copy(Parent.begin(), Parent.end(), Words.begin());
  const va_t End = Entry + Parent.size() * 4;
  Words[(Foo - Entry) / 4] = Ret;
  // adrp x16, ImportSlot; ldr x16, [x16]; br x16
  const uint32_t Stub[] = {0xb0000010, 0xf9400210, 0xd61f0200};
  std::copy(std::begin(Stub), std::end(Stub),
            Words.begin() + (Veneer - Entry) / 4);
  const std::vector<uint32_t> Handler = handlerCode(S);
  std::copy(Handler.begin(), Handler.end(),
            Words.begin() + (Funclet - Entry) / 4);
  const va_t HandlerEnd = Funclet + Handler.size() * 4;

  Segment Text;
  Text.Name = ".text";
  Text.VA = Entry;
  Text.Size = Words.size() * 4;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(Text.Size);
  std::memcpy(Text.Data.data(), Words.data(), Text.Size);
  Img.Segments.push_back(std::move(Text));
  Section Code;
  Code.Name = ".text";
  Code.VA = Entry;
  Code.Size = Words.size() * 4;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(Code));
  Segment Data;
  Data.Name = ".rdata";
  Data.VA = ImportSlot;
  Data.Size = 8;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.assign(Data.Size, 0);
  Img.Segments.push_back(std::move(Data));
  Img.Symbols.push_back(Symbol::makeFunc(Entry, End - Entry));
  Img.Symbols.push_back(Symbol::makeFunc(Foo, 4));
  Img.Symbols.push_back(Symbol::makeFunc(Funclet, HandlerEnd - Funclet));
  Import LocalUnwind;
  LocalUnwind.Name = "_local_unwind";
  LocalUnwind.Module = "VCRUNTIME140.dll";
  LocalUnwind.IATAddr = ImportSlot;
  Img.Imports.push_back(LocalUnwind);
  if (RegisterStub)
    EXPECT_TRUE(Img.recordImportStub(Veneer, 0));

  const bool X19 = S == Shape::X19Local || S == Shape::X19Slot;
  ExceptionFunction EH;
  EH.CodeRange = {Entry, End};
  EH.Encoding = ExceptionEncoding::ARM64Unpacked;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  // The codes run from the prologue's last instruction to its first.
  EH.UnwindOperations.push_back(
      unwindOp(UnwindOperationKind::SetFramePointer, 0, 29));
  if (X19)
    EH.UnwindOperations.push_back(
        unwindOp(UnwindOperationKind::SaveRegister, 0x10, 19));
  EH.UnwindOperations.push_back(unwindOp(
      UnwindOperationKind::SaveRegisterPairPreIndexed, X19 ? 0x30 : 0x20, 29));
  EH.UnwindOperations.push_back(unwindOp(UnwindOperationKind::End));
  EH.PrologueSize = 4 * (EH.UnwindOperations.size() - 1);
  EH.SEH.emplace();
  SEHScopeRecord Finally;
  Finally.GuardedRange =
      X19 ? ExceptionAddressRange{Entry + 0x1c, Entry + 0x40}
          : ExceptionAddressRange{Entry + 0x14, Entry + 0x38};
  Finally.Kind = SEHScopeKind::Finally;
  Finally.FilterOrFinallyVA = Funclet;
  Finally.HandlerVA = Funclet;
  EH.SEH->Scopes.push_back(Finally);
  Img.ExceptionMetadata.Functions.push_back(std::move(EH));

  ExceptionFunction Handled;
  Handled.CodeRange = {Funclet, HandlerEnd};
  Handled.Encoding = ExceptionEncoding::ARM64Unpacked;
  Handled.UnwindOperations.push_back(
      unwindOp(UnwindOperationKind::SaveRegisterPairPreIndexed, 0x10, 29));
  Handled.UnwindOperations.push_back(unwindOp(UnwindOperationKind::End));
  Handled.PrologueSize = 4;
  Img.ExceptionMetadata.Functions.push_back(std::move(Handled));
  Img.ExceptionMetadata.rebuildIndex();
  return Img;
}

struct Certified {
  std::vector<LowSEHLocalUnwind> Sites;
  std::vector<LowSEHLocalUnwindRefusal> Refused;
  std::string Error;
};

/// The protocol over the decoded parent and handler.
Certified certify(const BinaryImage &Img) {
  Decoder Dec;
  EXPECT_TRUE(Dec.init(Arch::AArch64));
  const LowFunc Parent = CFGBuilder().build(Img, Dec, Entry, "parent");
  const LowFunc Handler = CFGBuilder().build(Img, Dec, Funclet, "funclet");
  const std::map<va_t, const LowFunc *> Bodies{{Funclet, &Handler}};
  size_t Work = limits::kMaxSEHEstablisherFrameWork;
  Certified Result;
  auto Sites =
      certifyLowSEHLocalUnwinds(Parent, Img, getTargetRegInfo(Arch::AArch64),
                                Bodies, Work, nullptr, &Result.Refused);
  if (Sites)
    Result.Sites = std::move(*Sites);
  else
    Result.Error = llvm::toString(Sites.takeError());
  return Result;
}

// The whole translation unit as HighC.
std::string decompileHighC(const BinaryImage &Img) {
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  auto Result = Pipeline().run(Img, Ctx, Opts);
  EXPECT_TRUE(Result.Success) << Result.Error;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions COpts;
  COpts.TheArch = Arch::AArch64;
  COpts.Format = BinaryFormat::COFF;
  EXPECT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, COpts));
  OS.flush();
  return Source;
}

std::string parentText(const std::string &Source) {
  const size_t At = Source.find("sub_140001000(");
  if (At == std::string::npos)
    return {};
  const size_t Brace = Source.find('{', At);
  int Depth = 0;
  for (size_t I = Brace; I < Source.size(); ++I) {
    if (Source[I] == '{')
      ++Depth;
    else if (Source[I] == '}' && --Depth == 0)
      return Source.substr(At, I - At + 1);
  }
  return Source.substr(At);
}

size_t occurrences(const std::string &Text, const std::string &Needle) {
  size_t Count = 0;
  for (size_t At = Text.find(Needle); At != std::string::npos;
       At = Text.find(Needle, At + 1))
    ++Count;
  return Count;
}

TEST(ARM64SEHLocalUnwind, TheVeneerAndTheSavedEntryStackPointerCertify) {
  // Nothing records the veneer as the import's stub: its code names the
  // import slot.  x0 reloads the slot the prologue fills with the entry
  // stack pointer, x1 is the target, and the call runs on the body's stack
  // pointer; the handler receives the frame pointer.
  const Certified C =
      certify(makeImage(Shape::Guarded, /*RegisterStub=*/false));
  ASSERT_TRUE(C.Error.empty()) << C.Error;
  ASSERT_EQ(C.Sites.size(), 1u);
  EXPECT_TRUE(C.Refused.empty());
  const LowSEHLocalUnwind &Site = C.Sites.front();
  EXPECT_EQ(Site.CallAddr, Entry + 0x24);
  // The handler steps the return address back to the `bl`.
  EXPECT_EQ(Site.ControlPc, Entry + 0x24);
  EXPECT_EQ(Site.Target, Target);
  EXPECT_EQ(Site.TargetFrame, 0);
  // The frame pointer the handler receives is the body's stack pointer.
  EXPECT_FALSE(Site.HandlerFrameRegister.has_value());
  EXPECT_EQ(Site.HandlerFrameFromSP, 0);
  EXPECT_EQ(Site.ResumeReturnValue, 0u);
  EXPECT_TRUE(Site.OnlyExitsName);
}

TEST(ARM64SEHLocalUnwind, AReturnOutOfTheTryRunsTheFinallyOnce) {
  // The local unwind becomes the jump it is, which leaves the `__try`: C
  // runs the clause, with the handler's unwind work, on that jump, and the
  // compiler's copy on the normal path.  The handler runs once on each.
  for (const Shape S : {Shape::Guarded, Shape::X19Local}) {
    const std::string Full = decompileHighC(makeImage(S));
    const std::string P = parentText(Full);
    ASSERT_FALSE(P.empty()) << Full;
    EXPECT_EQ(P.find("_local_unwind"), std::string::npos) << P;
    EXPECT_EQ(P.find("0x140001048"), std::string::npos) << P;
    EXPECT_EQ(P.find("sub_1400010A0("), std::string::npos) << P;
    const size_t Try = P.find("__try {");
    const size_t Finally = P.find("} __finally {");
    ASSERT_NE(Try, std::string::npos) << P;
    ASSERT_NE(Finally, std::string::npos) << P;
    EXPECT_LT(P.find("goto ", Try), Finally) << P;
    EXPECT_NE(P.find("if (AbnormalTermination())", Finally), std::string::npos)
        << P;
    EXPECT_EQ(occurrences(P, "= 99"), 1u) << P;
    EXPECT_GT(P.find("= 99"), Finally) << P;
    EXPECT_EQ(occurrences(P, "= 100"), 1u) << P;
    EXPECT_GT(P.find("= 100"), Finally) << P;
  }
}

TEST(ARM64SEHLocalUnwind, ATargetThatReturnsX0ReturnsWhatTheRoutineLeaves) {
  // The routine resumes through `RtlUnwind` with no return value, as on x64:
  // a target that returns x0 unwritten returns that.
  const Certified C = certify(makeImage(Shape::ResultLive));
  ASSERT_TRUE(C.Error.empty()) << C.Error;
  ASSERT_EQ(C.Sites.size(), 1u);
  EXPECT_TRUE(C.Refused.empty());
  EXPECT_EQ(C.Sites.front().ResumeReturnValue, 0u);
  const std::string P =
      parentText(decompileHighC(makeImage(Shape::ResultLive)));
  ASSERT_FALSE(P.empty());
  EXPECT_EQ(P.find("_local_unwind("), std::string::npos) << P;
}

TEST(ARM64SEHLocalUnwind, AHandlerStoresThroughTheParentsX19) {
  // The runtime starts the handler with the parent's x19, its frame: the
  // store lands on a local, not on the saved-frame slot.
  const Certified C = certify(makeImage(Shape::X19Local));
  ASSERT_TRUE(C.Error.empty()) << C.Error;
  ASSERT_EQ(C.Sites.size(), 1u);
}

TEST(ARM64SEHLocalUnwind, AHandlerThatWritesTheSlotThroughX19KeepsTheCall) {
  const Certified C = certify(makeImage(Shape::X19Slot));
  EXPECT_NE(C.Error.find("writes the saved-frame slot"), std::string::npos)
      << C.Error;
  const std::string P = parentText(decompileHighC(makeImage(Shape::X19Slot)));
  ASSERT_FALSE(P.empty());
  EXPECT_NE(P.find("_local_unwind("), std::string::npos) << P;
}

} // namespace
