#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/X86/X87Context.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/CallRegisterEffects.h"
#include "neverd/ir/low/InternalNoReturn.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/support/ProloguePatterns.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <vector>

using namespace neverd;

namespace neverd::detail {
struct NoReturnProofTestAccess {
  static bool prove(CFGBuilder &Builder, const BinaryImage &Image, Decoder &Dec,
                    va_t Entry) {
    return Builder.buildNoReturnProof(Image, Dec, Entry, "proof");
  }
};
} // namespace neverd::detail

namespace {

constexpr va_t Base = 0x1000;

/// x64 code at Base, one function every 0x10 bytes.
BinaryImage imageWith(std::vector<uint8_t> Code,
                      std::vector<std::pair<va_t, const char *>> Functions) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  Image.Base = Base;
  Segment Text;
  Text.VA = Base;
  Text.Size = Code.size();
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data = std::move(Code);
  Image.Segments.push_back(std::move(Text));
  for (const auto &[Address, Name] : Functions) {
    auto Function = Symbol::makeFunc(Address);
    Function.Name = Name;
    Image.Symbols.push_back(Function);
  }
  return Image;
}

/// `call Target` at \p From, as e8 rel32.
void putCall(std::vector<uint8_t> &Code, va_t From, va_t Target) {
  const auto Offset = static_cast<size_t>(From - Base);
  const auto Rel = static_cast<int32_t>(Target - (From + 5));
  Code[Offset] = 0xe8;
  for (unsigned I = 0; I < 4; ++I)
    Code[Offset + 1 + I] = static_cast<uint8_t>(Rel >> (8 * I));
}

/// Whether the call that starts the function at Base ends its block, as
/// built with an InternalNoReturnIndex over \p Image.
bool callAtBaseEndsItsBlock(const BinaryImage &Image) {
  Decoder Dec;
  EXPECT_TRUE(Dec.init(Image.Arch));
  std::set<va_t> Entries;
  for (const auto &Sym : Image.Symbols)
    Entries.insert(Sym.Addr);
  const libc::NoReturnTargetIndex Names(Image);
  const InternalNoReturnIndex Callees(Image, &Entries, &Names, nullptr,
                                      nullptr);
  CFGBuilder Builder;
  Builder.setKnownFuncEntries(&Entries);
  Builder.setNoReturnTargetIndex(&Names);
  Builder.setNoReturnCalleeProver(&Callees);
  const LowFunc Caller = Builder.build(Image, Dec, Base, "caller");
  for (const LowBlock &Block : Caller.Blocks)
    for (const LowInstructionBoundary &Insn : Block.InstructionBoundaries)
      if (Insn.Address == Base)
        return hasLowInstructionControlFlag(
            Insn.ControlFlags, LowInstructionControlFlag::NoReturn);
  ADD_FAILURE() << "no instruction at the caller's entry";
  return false;
}

/// caller: call callee; int3; ret.  The callee at Base + 0x10 is \p Callee.
std::vector<uint8_t> callerThen(std::vector<uint8_t> Callee) {
  std::vector<uint8_t> Code(0x10, 0xcc);
  putCall(Code, Base, Base + 0x10);
  Code[6] = 0xc3;
  Code.insert(Code.end(), Callee.begin(), Callee.end());
  Code.resize(0x40, 0xcc);
  return Code;
}

// The root calls a forwarding wrapper whose leaf returns an initialized
// binary80 value. Counting queries about that leaf distinguishes the root's
// terminating-path proof from recursive floating-return recovery.
struct ReturningCallCounter final : NoReturnCalleeProver {
  mutable unsigned LeafQueries = 0;
  bool mayNeverReturn(va_t) const override { return true; }
  bool neverReturns(va_t Target, unsigned) const override {
    LeafQueries += Target == Base + 0x40;
    return false;
  }
};

BinaryImage floatingForwarder(Arch Architecture, BinaryFormat Format,
                              const std::vector<uint8_t> &Local, bool Trap) {
  std::vector<uint8_t> Code(0x60, 0xcc);
  putCall(Code, Base, Base + 0x20);
  std::copy(Local.begin(), Local.end(), Code.begin() + 5);
  const size_t End = 5 + Local.size();
  if (Trap) {
    Code[End] = 0xcd;
    Code[End + 1] = 0x29; // fast fail
  } else {
    Code[End] = 0xc3;
  }
  putCall(Code, Base + 0x20, Base + 0x40);
  Code[0x25] = 0xc3;
  Code[0x40] = 0xd9; // fld1; ret
  Code[0x41] = 0xe8;
  Code[0x42] = 0xc3;
  auto Image = imageWith(Code, {{Base, "root"},
                                {Base + 0x20, "forwarder"},
                                {Base + 0x40, "floating_leaf"}});
  Image.Arch = Architecture;
  Image.Bits = Architecture == Arch::X86 ? Bitness::Bits32 : Bitness::Bits64;
  Image.Format = Format;
  return Image;
}

// Whether \p Image's calling convention owns the x87 stack as one context,
// whose calls return through the context rather than an st0 value.
bool ownsX87(const BinaryImage &Image) {
  return x87ContextOwnsStack(Image.Arch, Image.abiFormat(),
                             Image.X87ContextOverride);
}

TEST(InternalNoReturn, TerminationDoesNotDemandUnusedFloatingReturns) {
  for (Arch Architecture : {Arch::X86, Arch::X64})
    for (BinaryFormat Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (bool Trap : {false, true}) {
        SCOPED_TRACE(static_cast<int>(Architecture));
        SCOPED_TRACE(static_cast<int>(Format));
        SCOPED_TRACE(Trap);
        const auto Image = floatingForwarder(Architecture, Format, {}, Trap);
        const std::set<va_t> Entries{Base, Base + 0x20, Base + 0x40};
        ReturningCallCounter Counter;
        Decoder Dec;
        ASSERT_TRUE(Dec.init(Image));
        CFGBuilder Builder;
        Builder.setKnownFuncEntries(&Entries);
        Builder.setNoReturnCalleeProver(&Counter);
        EXPECT_EQ(
            detail::NoReturnProofTestAccess::prove(Builder, Image, Dec, Base),
            Trap);
        EXPECT_EQ(Counter.LeafQueries, 0u);

        // Reusing the same builder for a normal projection restores full
        // floating-return recovery, including a wrapper with no local x87.
        const LowFunc Full = Builder.build(Image, Dec, Base, "full");
        ASSERT_TRUE(Full.hasCompleteLiftCoverage());
        EXPECT_EQ(lowFunctionNeverReturns(Full, Architecture), Trap);
        EXPECT_GT(Counter.LeafQueries, 0u);
        bool SawCall = false, SawReturn = false;
        for (const auto &Block : Full.Blocks)
          for (const LowOp &Op : Block.Ops) {
            if (Op.Opcode == NdOp::CALL && Op.Addr == Base) {
              SawCall = true;
              if (ownsX87(Image)) {
                EXPECT_EQ(Op.Output, NdVar::reg(x86reg::RAX, 8));
              } else {
                EXPECT_EQ(Op.Output.Size, x86reg::FPURegSize);
                EXPECT_TRUE(Op.Output.isReg());
                EXPECT_GE(Op.Output.Offset, x86reg::ST0);
                EXPECT_LE(Op.Output.Offset, x86reg::ST7);
              }
            }
            if (Op.Opcode == NdOp::RETURN) {
              SawReturn = true;
              if (ownsX87(Image)) {
                ASSERT_EQ(Op.NumInputs, 1u);
                EXPECT_EQ(Op.Inputs[0], NdVar::reg(x86reg::RAX, 8));
              } else {
                EXPECT_EQ(Op.NumInputs, 1u);
                EXPECT_EQ(Op.Inputs[0].Size, x86reg::FPURegSize);
              }
            }
          }
        EXPECT_TRUE(SawCall);
        EXPECT_EQ(SawReturn, !Trap);
      }
}

TEST(InternalNoReturn, LocalFloatingStateKeepsTheFullProof) {
  const std::vector<std::vector<uint8_t>> Locals{
      {0xd9, 0xe8, 0xdd, 0xd8}, // fld1; fstp st(0)
      {0xd9, 0xe5},             // fxam: exposes stack tags to status bits
      {0xd9, 0xf7},             // fincstp: rotates without reading ST(i)
      {0xdb, 0xe3}};            // fninit: resets TOP and tags
  for (auto [Architecture, Format] : {std::pair{Arch::X86, BinaryFormat::ELF},
                                      std::pair{Arch::X64, BinaryFormat::ELF},
                                      std::pair{Arch::X64, BinaryFormat::COFF}})
    for (const auto &Local : Locals)
      for (bool Trap : {false, true}) {
        SCOPED_TRACE(static_cast<int>(Architecture));
        SCOPED_TRACE(static_cast<int>(Format));
        SCOPED_TRACE(Local[1]);
        SCOPED_TRACE(Trap);
        const auto Image = floatingForwarder(Architecture, Format, Local, Trap);
        const std::set<va_t> Entries{Base, Base + 0x20, Base + 0x40};
        ReturningCallCounter Counter;
        Decoder Dec;
        ASSERT_TRUE(Dec.init(Image));
        CFGBuilder Builder;
        Builder.setKnownFuncEntries(&Entries);
        Builder.setNoReturnCalleeProver(&Counter);
        EXPECT_EQ(
            detail::NoReturnProofTestAccess::prove(Builder, Image, Dec, Base),
            Trap);
        if (ownsX87(Image))
          EXPECT_EQ(Counter.LeafQueries, 0u);
        else
          EXPECT_GT(Counter.LeafQueries, 0u);
        const LowFunc Full = Builder.build(Image, Dec, Base, "full");
        EXPECT_EQ(lowFunctionNeverReturns(Full, Architecture), Trap);
      }
}

TEST(InternalNoReturn, TerminationProjectionPreservesExceptionHandlers) {
  for (Arch Architecture : {Arch::X86, Arch::X64})
    for (unsigned Handler = 0; Handler != 3; ++Handler) {
      SCOPED_TRACE(static_cast<int>(Architecture));
      SCOPED_TRACE(Handler);
      auto Image = floatingForwarder(Architecture, BinaryFormat::ELF, {}, true);
      auto &Code = Image.Segments.front().Data;
      Code[0x10] = Handler == 1 ? 0xcd : 0xc3;
      Code[0x11] = 0x29;
      ExceptionFunction EH;
      EH.CodeRange = {Base, Base + 0x20};
      EH.Encoding = ExceptionEncoding::DwarfFDE;
      EH.Personality = ExceptionPersonality::GxxPersonalityV0;
      EH.Itanium.emplace();
      ItaniumCallSite Site;
      Site.GuardedRange = {Base, Base + 5};
      Site.LandingPadVA = Handler == 2 ? Base + 0x100 : Base + 0x10;
      EH.Itanium->CallSites.push_back(Site);
      Image.ExceptionMetadata.Functions.push_back(std::move(EH));
      Image.ExceptionMetadata.rebuildIndex();
      const std::set<va_t> Entries{Base, Base + 0x20, Base + 0x40};
      Decoder Dec;
      ASSERT_TRUE(Dec.init(Image));
      CFGBuilder Builder;
      Builder.setKnownFuncEntries(&Entries);
      EXPECT_EQ(
          detail::NoReturnProofTestAccess::prove(Builder, Image, Dec, Base),
          Handler == 1);
      const LowFunc Full = Builder.build(Image, Dec, Base, "full");
      EXPECT_EQ(lowFunctionNeverReturns(Full, Architecture), Handler == 1);
    }
}

TEST(InternalNoReturn, UnknownTagsRequireScalarProofOrOwnedRuntimeContext) {
  for (Arch Architecture : {Arch::X86, Arch::X64})
    for (BinaryFormat Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
      SCOPED_TRACE(static_cast<int>(Architecture));
      SCOPED_TRACE(static_cast<int>(Format));
      auto Image =
          floatingForwarder(Architecture, Format, {0xeb, 0, 0xd9, 0xe5}, true);
      // An independent entry at FXAM cannot inherit the call's known full
      // slot. A termination proof must not make that value usable in LowIR.
      Image.CodeRefTargets.insert(Base + 7);
      const std::set<va_t> Entries{Base, Base + 0x20, Base + 0x40};
      Decoder Dec;
      ASSERT_TRUE(Dec.init(Image));
      CFGBuilder Builder;
      Builder.setKnownFuncEntries(&Entries);
      const bool Proof =
          detail::NoReturnProofTestAccess::prove(Builder, Image, Dec, Base);
      const LowFunc Full = Builder.build(Image, Dec, Base, "full");
      const bool Owned = ownsX87(Image);
      EXPECT_EQ(Full.hasCompleteInstructionLift(), Owned);
      EXPECT_EQ(Full.UnsupportedInstructionAddresses,
                Owned ? std::vector<va_t>{} : std::vector<va_t>{Base + 7});
      if (Owned) {
        unsigned RootCapture = 0;
        for (const auto &B : Full.Blocks)
          for (const auto &Op : B.Ops)
            RootCapture +=
                B.StartAddr == Base + 7 && Op.Opcode == NdOp::INTRINSIC &&
                Op.NumInputs == 1 &&
                Op.Inputs[0].Offset == unsigned(Intrinsic::X87ContextBegin);
        EXPECT_EQ(RootCapture, 1u);
      }
      EXPECT_EQ(Proof, lowFunctionNeverReturns(Full, Architecture));
      const InternalNoReturnIndex Index(Image, &Entries, nullptr, nullptr,
                                        nullptr);
      EXPECT_EQ(Index.neverReturns(Base, 0), Proof);
    }
}

TEST(InternalNoReturn, NoReturnInstructionStillFollowsExceptionalPaths) {
  for (const auto Kind :
       {ExceptionalEdgeKind::CxxCatch, ExceptionalEdgeKind::SEHHandler,
        ExceptionalEdgeKind::ItaniumCatchPad, ExceptionalEdgeKind::GoRecover}) {
    SCOPED_TRACE(static_cast<int>(Kind));
    for (unsigned Destination = 0; Destination != 4; ++Destination) {
      SCOPED_TRACE(Destination);
      auto NoReturnBlock = [](int Id) {
        LowBlock Block;
        Block.Id = Id;
        Block.StartAddr = Base + 0x10 * Id;
        Block.EndAddr = Block.StartAddr + 5;
        LowInstructionBoundary Insn;
        Insn.Address = Block.StartAddr;
        Insn.Size = 5;
        Insn.ControlFlags = LowInstructionControlFlag::NoReturn;
        Block.InstructionBoundaries.push_back(Insn);
        return Block;
      };
      auto Entry = NoReturnBlock(0);
      Entry.Succs.push_back(2);
      Entry.ExceptionalSuccs.push_back({Destination == 2   ? -1
                                        : Destination == 3 ? 99
                                                           : 1,
                                        Base + 0x10, Kind});
      auto Handler = NoReturnBlock(1);
      auto Returning = NoReturnBlock(2);
      LowOp Return;
      Return.Opcode = NdOp::RETURN;
      Returning.InstructionBoundaries.clear();
      Returning.Ops.push_back(Return);
      if (Destination != 1) {
        Handler.InstructionBoundaries.clear();
        Handler.Ops.push_back(Return);
      }
      LowFunc Function;
      Function.Entry = Base;
      Function.Blocks = {Entry, Handler, Returning};
      EXPECT_EQ(lowFunctionNeverReturns(Function, Arch::X86), Destination == 1);
    }
  }
}

TEST(InternalNoReturn, CallThroughALoaderBoundNoReturnSlotEndsItsBlock) {
  // `_start: call *__libc_start_main@GOT(%rip); hlt`. The import the loader
  // binds to the slot is the callee; nothing after the call runs.
  for (const char *Bound : {"__libc_start_main", "puts"}) {
    SCOPED_TRACE(Bound);
    std::vector<uint8_t> Code(0x40, 0xcc);
    // call qword ptr [rip + 0x1ffa] (the slot at 0x3000); hlt
    const uint8_t Call[] = {0xff, 0x15, 0xfa, 0x1f, 0x00, 0x00, 0xf4};
    std::copy(std::begin(Call), std::end(Call), Code.begin());
    auto Image = imageWith(Code, {{Base, "_start"}});
    Segment Got;
    Got.VA = 0x3000;
    Got.Size = Got.FileSz = 8;
    Got.Data.resize(8);
    Got.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Image.Segments.push_back(std::move(Got));
    ASSERT_TRUE(Image.recordImportStorageSlot(
        0x3000, Bound, 0, ImportStorageEvidence::LoaderBind));
    EXPECT_EQ(callAtBaseEndsItsBlock(Image),
              std::string_view(Bound) == "__libc_start_main");
  }
}

TEST(InternalNoReturn, CallToACalleeEndingInFastFailEndsItsBlock) {
  // callee: int 0x29.
  const auto Image =
      imageWith(callerThen({0xcd, 0x29}), {{Base, "caller"}, {0x1010, "b"}});
  EXPECT_TRUE(callAtBaseEndsItsBlock(Image));
}

TEST(InternalNoReturn, CalleeThatCanReturnKeepsTheFallThrough) {
  // callee: test ecx, ecx; jz +2; int 0x29; ret -- one path returns.
  const auto Image =
      imageWith(callerThen({0x85, 0xc9, 0x74, 0x02, 0xcd, 0x29, 0xc3}),
                {{Base, "caller"}, {0x1010, "b"}});
  EXPECT_FALSE(callAtBaseEndsItsBlock(Image));
}

TEST(InternalNoReturn, CalleeEndingInAKnownNoReturnCallEndsItsBlock) {
  // callee: call abort; int3.  abort is in the no-return name list.
  std::vector<uint8_t> Callee(0x10, 0xcc);
  auto Code = callerThen(Callee);
  putCall(Code, Base + 0x10, Base + 0x30);
  Code[0x30] = 0xc3;
  const auto Image =
      imageWith(Code, {{Base, "caller"}, {0x1010, "b"}, {0x1030, "abort"}});
  EXPECT_TRUE(callAtBaseEndsItsBlock(Image));
}

TEST(InternalNoReturn, RustNeverReturningRoutinesEndTheirCallersBlock) {
  // Rust's standard library declares its panic entry points `-> !`.  A body
  // that reaches the runtime through more calls than a proof follows (here
  // through a pointer, then `ret`) is known by its name, as `abort` is; the
  // bytes after the call can be a jump table rustc keeps in __text.
  const std::vector<uint8_t> CallsThroughAPointer = {0xff, 0xd0, 0xc3};
  for (const char *Name :
       {"_RNvNtCs1a2b3c4d5e6_4core6option13unwrap_failed",
        "__RNvNtCsbAqs9W1eE8G_4core9panicking16panic_in_cleanup",
        "__RINvNtCsbAqs9W1eE8G_4core9panicking13assert_failedyyEB4_",
        "_ZN4core9panicking5panic17h0123456789abcdefE"}) {
    SCOPED_TRACE(Name);
    EXPECT_TRUE(callAtBaseEndsItsBlock(imageWith(
        callerThen(CallsThroughAPointer), {{Base, "caller"}, {0x1010, Name}})));
  }
  // What only inspects a panic returns, and so does the runtime's attempt to
  // start one when no unwinder takes it.
  for (const char *Name :
       {"_RNvNtCsgejaSCmAoRz_3std6thread9panicking",
        "__RNvCs9wFQrvczXsK_7___rustc18___rust_start_panic"}) {
    SCOPED_TRACE(Name);
    EXPECT_FALSE(callAtBaseEndsItsBlock(imageWith(
        callerThen(CallsThroughAPointer), {{Base, "caller"}, {0x1010, Name}})));
  }
}

TEST(InternalNoReturn, WhatTheUserStatesOfACalleeDecides) {
  // As IDA's Edit function does, a statement decides over the callee's name
  // and its body, in either direction.
  {
    SCOPED_TRACE("a returning callee stated never to return");
    auto Image =
        imageWith(callerThen({0xc3}), {{Base, "caller"}, {0x1010, "b"}});
    EXPECT_FALSE(callAtBaseEndsItsBlock(Image));
    Image.UserNoReturn[0x1010] = true;
    EXPECT_TRUE(callAtBaseEndsItsBlock(Image));
    EXPECT_TRUE(libc::isNoReturnTarget(Image, 0x1010));
    EXPECT_TRUE(libc::NoReturnTargetIndex(Image).contains(Image, 0x1010));
  }
  {
    SCOPED_TRACE("abort stated to return");
    auto Image =
        imageWith(callerThen({0xc3}), {{Base, "caller"}, {0x1010, "abort"}});
    EXPECT_TRUE(callAtBaseEndsItsBlock(Image));
    Image.UserNoReturn[0x1010] = false;
    EXPECT_FALSE(callAtBaseEndsItsBlock(Image));
    EXPECT_FALSE(libc::isNoReturnTarget(Image, 0x1010));
    EXPECT_FALSE(libc::NoReturnTargetIndex(Image).contains(Image, 0x1010));
  }
  {
    SCOPED_TRACE("a fast-failing callee stated to return");
    auto Image =
        imageWith(callerThen({0xcd, 0x29}), {{Base, "caller"}, {0x1010, "b"}});
    EXPECT_TRUE(callAtBaseEndsItsBlock(Image));
    Image.UserNoReturn[0x1010] = false;
    EXPECT_FALSE(callAtBaseEndsItsBlock(Image));
  }
}

/// caller: call b1; ret.  Each of b1 .. bN is `call next; ret`, sized so the
/// screen decodes it, and the last calls c: ret.  Code follows every call
/// and every b holds a `ret`, so only what the user states of c can end the
/// caller's call.
BinaryImage statedChain(unsigned Length) {
  std::vector<uint8_t> Code(0x10 * (Length + 2), 0xcc);
  std::vector<std::pair<va_t, const char *>> Functions{{Base, "caller"}};
  static const char *Names[] = {"b1", "b2", "b3", "b4", "b5", "b6"};
  for (unsigned I = 0; I <= Length; ++I) {
    const va_t At = Base + 0x10 * I;
    putCall(Code, At, At + 0x10);
    Code[At - Base + 5] = 0xc3;
    if (I != 0)
      Functions.push_back({At, Names[I - 1]});
  }
  const va_t C = Base + 0x10 * (Length + 1);
  Code[C - Base] = 0xc3;
  Functions.push_back({C, "c"});
  auto Image = imageWith(Code, Functions);
  for (Symbol &Sym : Image.Symbols)
    Sym.Size = Sym.Addr == C ? 1 : 6;
  return Image;
}

TEST(InternalNoReturn, CalleeCallingWhatTheUserStatesNeverReturnsIsProved) {
  // As IDA passes an edited no-return flag on to callers: once c is stated
  // never to return, b's `ret` is unreachable, so b is proved and the call
  // to it ends its block.  A statement that b returns decides over that.
  auto Image = statedChain(1);
  const va_t B = Base + 0x10, C = Base + 0x20;
  EXPECT_FALSE(callAtBaseEndsItsBlock(Image));
  Image.UserNoReturn[C] = true;
  EXPECT_TRUE(callAtBaseEndsItsBlock(Image));
  Image.UserNoReturn[B] = false;
  EXPECT_FALSE(callAtBaseEndsItsBlock(Image));
}

TEST(InternalNoReturn, ScreenWantsEveryReturnAfterACallThatCannotComeBack) {
  // Stating c never returns sends to the proof only callees whose every
  // return follows, with no branch in between, a call that cannot come back:
  // a call to c, or to a function with no return that calls c.  A callee
  // that calls c on one path and returns on another needs no proof.
  std::vector<uint8_t> Code(0x120, 0xcc);
  const va_t C = Base + 0x100, D = Base + 0x110, W = Base + 0x40;
  const auto put = [&](va_t At, std::initializer_list<uint8_t> Bytes) {
    std::copy(Bytes.begin(), Bytes.end(), Code.begin() + (At - Base));
  };
  putCall(Code, Base, C); // b1: call c; ret
  put(Base + 5, {0xc3});
  put(Base + 0x10, {0x85, 0xc9, 0x74, 0x05}); // b2: test; jz over; call c
  putCall(Code, Base + 0x14, C);
  put(Base + 0x19, {0xc3});      //     over: ret
  putCall(Code, Base + 0x20, C); // b3: call c; add rsp, 8; ret
  put(Base + 0x25, {0x48, 0x83, 0xc4, 0x08, 0xc3});
  putCall(Code, Base + 0x30, C); // b4: call c; call d; ret
  putCall(Code, Base + 0x35, D);
  put(Base + 0x3a, {0xc3});
  putCall(Code, W, C);           // w: call c; int3
  putCall(Code, Base + 0x50, W); // b5: call w; ret
  put(Base + 0x55, {0xc3});
  putCall(Code, Base + 0x60, D); // b6: call d; ret
  put(Base + 0x65, {0xc3});
  put(C, {0xc3});
  put(D, {0xc3});
  auto Image = imageWith(Code, {{Base, "b1"},
                                {Base + 0x10, "b2"},
                                {Base + 0x20, "b3"},
                                {Base + 0x30, "b4"},
                                {W, "w"},
                                {Base + 0x50, "b5"},
                                {Base + 0x60, "b6"},
                                {C, "c"},
                                {D, "d"}});
  const std::map<std::string, uint64_t> Sizes = {
      {"b1", 6}, {"b2", 10}, {"b3", 10}, {"b4", 11}, {"w", 6},
      {"b5", 6}, {"b6", 6},  {"c", 1},   {"d", 1}};
  for (Symbol &Sym : Image.Symbols)
    Sym.Size = Sizes.at(Sym.Name);
  const auto screened = [&](const BinaryImage &In) {
    std::set<va_t> Entries;
    for (const Symbol &Sym : In.Symbols)
      Entries.insert(Sym.Addr);
    const libc::NoReturnTargetIndex Names(In);
    const InternalNoReturnIndex Index(In, &Entries, &Names, nullptr, nullptr);
    std::string Passed;
    for (const Symbol &Sym : In.Symbols)
      if (Index.mayNeverReturn(Sym.Addr))
        Passed += Sym.Name + " ";
    return Passed;
  };
  // Without a statement only w, which holds no return, is worth a proof.
  EXPECT_EQ(screened(Image), "w ");
  Image.UserNoReturn[C] = true;
  EXPECT_EQ(screened(Image), "b1 b3 w b5 ");
}

TEST(InternalNoReturn, ScreenTreatsAnUnliftableByteAsUnknown) {
  // b: call c; getsec; ret, with c stated never to return. GETSEC remains
  // pending without a register proof, so the screen cannot know b's
  // branches. It declines the proof instead of assuming local completion.
  std::vector<uint8_t> Code(0x30, 0xcc);
  putCall(Code, Base, Base + 0x20);
  Code[5] = 0x0f;
  Code[6] = 0x37;
  Code[7] = 0xc3;
  Code[0x20] = 0xc3;
  auto Image = imageWith(Code, {{Base, "b"}, {Base + 0x20, "c"}});
  for (Symbol &Sym : Image.Symbols)
    Sym.Size = Sym.Name == "b" ? 8 : 1;
  Image.UserNoReturn[Base + 0x20] = true;
  const std::set<va_t> Entries{Base, Base + 0x20};
  const libc::NoReturnTargetIndex Names(Image);
  const InternalNoReturnIndex Index(Image, &Entries, &Names, nullptr, nullptr);
  bool Screened = true;
  EXPECT_NO_THROW(Screened = Index.mayNeverReturn(Base));
  EXPECT_FALSE(Screened);
}

TEST(InternalNoReturn, StatedNoReturnReachesCallersUpToTheDepthLimit) {
  // A chain of such callees is proved to the same depth as one ending in a
  // trap (ProofsStopAtTheDepthLimit), and no further.
  for (unsigned Length :
       {limits::kMaxNoReturnProofDepth, limits::kMaxNoReturnProofDepth + 1}) {
    SCOPED_TRACE(Length);
    auto Image = statedChain(Length);
    Image.UserNoReturn[Base + 0x10 * (Length + 1)] = true;
    EXPECT_EQ(callAtBaseEndsItsBlock(Image),
              Length <= limits::kMaxNoReturnProofDepth);
  }
}

TEST(InternalNoReturn, PathAroundAStatedCallStillReturns) {
  // b: test ecx, ecx; jz over; call c; over: ret -- c is stated never to
  // return, but the path around the call still returns.
  std::vector<uint8_t> Code(0x30, 0xcc);
  putCall(Code, Base, Base + 0x10);
  Code[5] = 0xc3;
  const uint8_t Body[] = {0x85, 0xc9, 0x74, 0x05};
  std::copy(std::begin(Body), std::end(Body), Code.begin() + 0x10);
  putCall(Code, Base + 0x14, Base + 0x20);
  Code[0x19] = 0xc3;
  Code[0x20] = 0xc3;
  auto Image =
      imageWith(Code, {{Base, "caller"}, {0x1010, "b"}, {0x1020, "c"}});
  for (Symbol &Sym : Image.Symbols)
    Sym.Size = Sym.Name == "caller" ? 6 : Sym.Name == "b" ? 10 : 1;
  Image.UserNoReturn[0x1020] = true;
  EXPECT_FALSE(callAtBaseEndsItsBlock(Image));
}

TEST(InternalNoReturn, TailJumpIntoAFunctionThatNeverReturnsIsProved) {
  // b: jmp c, the shape a compiler gives a wrapper ending in its call when
  // it does not know c never returns.  The jump ends b's path as a call to
  // c would: once c is stated never to return, when the name list knows c,
  // or when c is proved, and a statement that c returns decides over both.
  struct Case {
    const char *Name;
    std::vector<uint8_t> Body;
  };
  for (const Case &C :
       {Case{"c", {0xc3}}, Case{"abort", {0xc3}}, Case{"c", {0xcd, 0x29}}}) {
    SCOPED_TRACE(std::string(C.Name) + " " + std::to_string(C.Body.size()));
    std::vector<uint8_t> Code(0x30, 0xcc);
    putCall(Code, Base, Base + 0x10);
    Code[5] = 0xc3;
    Code[0x10] = 0xe9;
    const auto Rel = static_cast<int32_t>((Base + 0x20) - (Base + 0x15));
    for (unsigned I = 0; I < 4; ++I)
      Code[0x11 + I] = static_cast<uint8_t>(Rel >> (8 * I));
    std::copy(C.Body.begin(), C.Body.end(), Code.begin() + 0x20);
    auto Image =
        imageWith(Code, {{Base, "caller"}, {0x1010, "b"}, {0x1020, C.Name}});
    for (Symbol &Sym : Image.Symbols)
      Sym.Size = Sym.Name == "caller" ? 6 : Sym.Name == "b" ? 5 : C.Body.size();
    const bool Known = std::string_view(C.Name) == "abort" || C.Body.size() > 1;
    EXPECT_EQ(callAtBaseEndsItsBlock(Image), Known);
    Image.UserNoReturn[0x1020] = !Known;
    EXPECT_EQ(callAtBaseEndsItsBlock(Image), !Known);
  }
}

TEST(InternalNoReturn, MutualRecursionWithoutATrapProvesNothing) {
  // callee: call caller; int3 -- a cycle with no terminating fact.
  std::vector<uint8_t> Callee(0x10, 0xcc);
  auto Code = callerThen(Callee);
  putCall(Code, Base + 0x10, Base);
  const auto Image = imageWith(Code, {{Base, "caller"}, {0x1010, "b"}});
  EXPECT_FALSE(callAtBaseEndsItsBlock(Image));
}

TEST(InternalNoReturn, ProofsStopAtTheDepthLimit) {
  // A chain of callees, each `call next; int3`, ending in `int 0x29`: one
  // within the depth limit is proved, one past it is not.
  for (unsigned Length :
       {limits::kMaxNoReturnProofDepth, limits::kMaxNoReturnProofDepth + 1}) {
    SCOPED_TRACE(Length);
    std::vector<uint8_t> Code(0x10 * (Length + 2), 0xcc);
    std::vector<std::pair<va_t, const char *>> Functions{{Base, "caller"}};
    static const char *Names[] = {"b1", "b2", "b3", "b4", "b5", "b6"};
    putCall(Code, Base, Base + 0x10);
    for (unsigned I = 1; I <= Length; ++I) {
      const va_t At = Base + 0x10 * I;
      Functions.push_back({At, Names[I - 1]});
      if (I == Length) {
        Code[At - Base] = 0xcd;
        Code[At - Base + 1] = 0x29;
      } else {
        putCall(Code, At, At + 0x10);
      }
    }
    const auto Image = imageWith(Code, Functions);
    EXPECT_EQ(callAtBaseEndsItsBlock(Image),
              Length <= limits::kMaxNoReturnProofDepth);
  }
}

TEST(InternalNoReturn, TrimmingKeepsAnswersUntilThereAreTooMany) {
  // A later run keeps the answers earlier runs found, which depend only on
  // the index's inputs.  Changing the callee after its proof, which no
  // session does, shows which answer a run reads.
  auto Image =
      imageWith(callerThen({0xcd, 0x29}), {{Base, "caller"}, {0x1010, "b"}});
  const std::set<va_t> Entries{Base, 0x1010};
  const libc::NoReturnTargetIndex Names(Image);
  InternalNoReturnIndex Callees(Image, &Entries, &Names, nullptr, nullptr);
  ASSERT_TRUE(Callees.neverReturns(0x1010, 0));
  Image.Segments.front().Data[0x10] = 0xc3;
  Callees.trimMemoizedProofs(limits::kMaxRetainedNoReturnAnswers);
  EXPECT_TRUE(Callees.neverReturns(0x1010, 0));
  // Past the bound the answers start over, as a cleared index does.
  Callees.trimMemoizedProofs(0);
  EXPECT_FALSE(Callees.neverReturns(0x1010, 0));
  Image.Segments.front().Data[0x10] = 0xcd;
  Callees.clearMemoizedProofs();
  EXPECT_TRUE(Callees.neverReturns(0x1010, 0));
}

TEST(InternalNoReturn, CallFollowedByCodeIsNotProved) {
  // caller: call callee; ret -- no padding follows and no metadata gives the
  // callee a code range to check, so the callee that never returns is not
  // lifted and the fall-through stays.
  std::vector<uint8_t> Code(0x10, 0xcc);
  putCall(Code, Base, Base + 0x10);
  Code[5] = 0xc3;
  Code.insert(Code.end(), {0xcd, 0x29});
  Code.resize(0x20, 0xcc);
  const auto Image = imageWith(Code, {{Base, "caller"}, {0x1010, "b"}});
  EXPECT_FALSE(callAtBaseEndsItsBlock(Image));
}

TEST(InternalNoReturn, RecognizesX86AlignmentNoOps) {
  struct Case {
    std::vector<uint8_t> Bytes;
    bool Is64Bit;
    size_t Length;
  };
  const Case Cases[] = {
      {{0x90}, true, 1},
      {{0x66, 0x90}, true, 2},
      {{0x0f, 0x1f, 0x00}, true, 3},
      {{0x0f, 0x1f, 0x40, 0x00}, true, 4},
      {{0x0f, 0x1f, 0x44, 0x00, 0x00}, true, 5},
      {{0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00}, true, 6},
      {{0x0f, 0x1f, 0x80, 0x00, 0x00, 0x00, 0x00}, true, 7},
      {{0x66, 0x66, 0x2e, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
       true,
       11},
      // The 32-bit fills that copy a register onto itself.
      {{0x89, 0xf6}, false, 2},
      {{0x8b, 0xff}, false, 2},
      {{0x8d, 0x76, 0x00}, false, 3},
      {{0x8d, 0x74, 0x26, 0x00}, false, 4},
      {{0x8d, 0xb4, 0x26, 0x00, 0x00, 0x00, 0x00}, false, 7},
      // In 64-bit code they zero the upper half.
      {{0x8d, 0x76, 0x00}, true, 0},
      // Not no-ops: pause, `nop` with a nonzero reg field, an adding lea, a
      // copy between two registers, and a truncated `nop r/m`.
      {{0xf3, 0x90}, true, 0},
      {{0x0f, 0x1f, 0x48, 0x00}, true, 0},
      {{0x8d, 0x76, 0x01}, false, 0},
      {{0x89, 0xf7}, false, 0},
      {{0x0f, 0x1f, 0x84, 0x00}, true, 0},
  };
  for (const Case &C : Cases)
    EXPECT_EQ(x86AlignmentNopLength(C.Bytes.data(), C.Bytes.size(), C.Is64Bit),
              C.Length);
}

TEST(InternalNoReturn, CallFollowedByAlignmentNoOpsEndsItsBlock) {
  // caller: call b, then GCC's no-ops up to b's entry; b: int 0x29.
  std::vector<uint8_t> Code(0x10, 0xcc);
  putCall(Code, Base, Base + 0x10);
  const uint8_t NoOps[] = {0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00,
                           0x0f, 0x1f, 0x44, 0x00, 0x00};
  std::copy(std::begin(NoOps), std::end(NoOps), Code.begin() + 5);
  Code.insert(Code.end(), {0xcd, 0x29});
  Code.resize(0x20, 0xcc);
  const auto Image = imageWith(Code, {{Base, "caller"}, {0x1010, "b"}});
  EXPECT_TRUE(callAtBaseEndsItsBlock(Image));
}

TEST(InternalNoReturn, AlignmentNoOpsBeforeTheCallersOwnCodeProveNothing) {
  // caller: call b; nop; ret at an aligned label of its own -- falling
  // through the no-op reaches the label, so it is no padding.  b at 0x1020.
  std::vector<uint8_t> Code(0x20, 0xcc);
  putCall(Code, Base, Base + 0x20);
  Code[5] = 0x0f;
  Code[6] = 0x1f;
  Code[7] = 0x00;
  Code[8] = 0xc3;
  Code.insert(Code.end(), {0xcd, 0x29});
  Code.resize(0x30, 0xcc);
  const auto Image = imageWith(Code, {{Base, "caller"}, {0x1020, "b"}});
  EXPECT_FALSE(callAtBaseEndsItsBlock(Image));
}

TEST(InternalNoReturn, CallEndingItsFunctionsCodeRangeEndsItsBlock) {
  // caller: call b; ret -- the caller's sized symbol ends with the call, so
  // the `ret` is not its code and the call does not return where it is made,
  // though b can (error(3) with a nonzero status is such a callee).  Without
  // the size, the fall-through stays.
  for (bool Sized : {true, false}) {
    SCOPED_TRACE(Sized);
    std::vector<uint8_t> Code(0x10, 0xcc);
    putCall(Code, Base, Base + 0x10);
    Code[5] = 0xc3;
    Code.insert(Code.end(), {0xc3});
    Code.resize(0x20, 0xcc);
    auto Image = imageWith(Code, {{Base, "caller"}, {0x1010, "b"}});
    if (Sized)
      Image.Symbols.front().Size = 5;
    EXPECT_EQ(callAtBaseEndsItsBlock(Image), Sized);
  }
}

TEST(InternalNoReturn, CallerWithAnUnresolvedBranchStillListsItsCallees) {
  // caller: call b; int3.  A switch the caller cannot resolve leaves its own
  // effect unknown, but b's summary does not depend on it: the caller still
  // lists b, so b is lifted and the call passes b's parameters.
  const auto Image =
      imageWith(callerThen({0xc3}), {{Base, "caller"}, {0x1010, "b"}});
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Image.Arch));
  std::set<va_t> Entries{Base, 0x1010};
  CFGBuilder Builder;
  Builder.setKnownFuncEntries(&Entries);
  LowFunc Caller = Builder.build(Image, Dec, Base, "caller");
  Caller.UnsafeIndirectBranchAddresses.insert(Base);
  const LocalRegisterEffect Effect =
      localRegisterEffect(Image, Caller, nullptr);
  EXPECT_TRUE(Effect.Incomplete);
  EXPECT_TRUE(Effect.Unknown);
  EXPECT_EQ(Effect.Callees, std::set<va_t>{0x1010});
}

TEST(InternalNoReturn, CallToACalleeWithNothingThatReturnsIsProved) {
  // caller: call b; ret -- code follows the call, but b's sized symbol
  // covers only `int 0x29`, which does not return, so b is proved.  A `ret`
  // that only the `int 0x29` precedes is reached by no path, and b is still
  // proved. A `ret` a branch reaches is worth no proof.
  struct Case {
    std::vector<uint8_t> Body;
    bool Proved;
  };
  for (const Case &C :
       {Case{{0xcd, 0x29}, true}, Case{{0xcd, 0x29, 0xc3}, true},
        Case{{0x74, 0x02, 0xcd, 0x29, 0xc3}, false}}) {
    SCOPED_TRACE(testing::PrintToString(C.Body));
    std::vector<uint8_t> Code(0x10, 0xcc);
    putCall(Code, Base, Base + 0x10);
    Code[5] = 0xc3;
    Code.insert(Code.end(), C.Body.begin(), C.Body.end());
    Code.resize(0x20, 0xcc);
    auto Image = imageWith(Code, {{Base, "caller"}, {0x1010, "b"}});
    Image.Symbols.back().Size = C.Body.size();
    EXPECT_EQ(callAtBaseEndsItsBlock(Image), C.Proved);
  }
}

TEST(InternalNoReturn, CalleeThatLeavesByAJumpIsNotProvedByItsScreen) {
  // caller: call b; ret -- b holds no `ret`, so the screen sends it to the
  // proof, but b leaves by a jump: to c, which returns, or through RAX to
  // code no one knows.  Either may come back, so the call keeps its
  // fall-through.
  const std::vector<std::vector<uint8_t>> Bodies = {
      {0xe9, 0x0b, 0x00, 0x00, 0x00}, // jmp c (b + 0x10)
      {0xff, 0xe0}};                  // jmp rax
  for (const std::vector<uint8_t> &Body : Bodies) {
    SCOPED_TRACE(Body.size());
    std::vector<uint8_t> Code(0x10, 0xcc);
    putCall(Code, Base, Base + 0x10);
    Code[5] = 0xc3;
    Code.insert(Code.end(), Body.begin(), Body.end());
    Code.resize(0x20, 0xcc);
    Code.push_back(0xc3); // c: ret
    Code.resize(0x30, 0xcc);
    auto Image =
        imageWith(Code, {{Base, "caller"}, {0x1010, "b"}, {0x1020, "c"}});
    for (Symbol &Sym : Image.Symbols)
      if (Sym.Name == "b")
        Sym.Size = Body.size();
    EXPECT_FALSE(callAtBaseEndsItsBlock(Image));
  }
}

TEST(InternalNoReturn, SummaryStillLiftsAProvedCalleeForItsParameters) {
  // caller: call b; int3, with b ending in `int 0x29`.  b's writes reach no
  // caller, but its reads give the call its parameters, so the summary lifts
  // it apart from the callees whose writes count.  A callee the name list
  // knows keeps its prototype and is not lifted.
  for (const char *Name : {"b", "abort"}) {
    SCOPED_TRACE(Name);
    const auto Image =
        imageWith(callerThen({0xcd, 0x29}), {{Base, "caller"}, {0x1010, Name}});
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Image.Arch));
    std::set<va_t> Entries{Base, 0x1010};
    const libc::NoReturnTargetIndex Names(Image);
    const InternalNoReturnIndex Callees(Image, &Entries, &Names, nullptr,
                                        nullptr);
    CFGBuilder Builder;
    Builder.setKnownFuncEntries(&Entries);
    Builder.setNoReturnTargetIndex(&Names);
    Builder.setNoReturnCalleeProver(&Callees);
    const LowFunc Caller = Builder.build(Image, Dec, Base, "caller");
    const LocalRegisterEffect Effect =
        localRegisterEffect(Image, Caller, &Names);
    EXPECT_FALSE(Effect.Callees.count(0x1010));
    EXPECT_EQ(Effect.NoReturnCallees.count(0x1010),
              std::string_view(Name) == "b" ? 1u : 0u);
  }
}

} // namespace
