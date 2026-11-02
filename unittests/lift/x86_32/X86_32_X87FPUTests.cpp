#include "../../LLVMHostFixture.h"
#include "NeverDLiftFixture.h"

#include "neverd/Limits.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/llvm/X86X87ImportCall.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/X87Context.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/CallRegisterEffects.h"
#include "neverd/ir/low/ImportCallee.h"
#include "neverd/ir/low/InternalNoReturn.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/COFF/COFFLoaderUtils.h"
#include "neverd/loader/PointerRelocation.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/Parallel.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <set>
#include <stdexcept>

namespace neverd::detail {
struct X87CallGraphCacheTestAccess {
  static std::array<size_t, 3> stats(const CFGBuilder &Builder) {
    return Builder.x87CallGraphCacheStatsForTesting();
  }
  static std::array<size_t, 5> flightStats(const CFGBuilder &Builder) {
    return Builder.x87CallGraphFlightStatsForTesting();
  }
  static std::array<size_t, 2> summaryStats(const CFGBuilder &Builder) {
    return Builder.callRegisterBodyCacheStatsForTesting();
  }
  static bool waitForWaiters(const CFGBuilder &Builder, size_t Count) {
    return Builder.waitForX87CallGraphWaitersForTesting(Count);
  }
  static std::pair<std::optional<int>, size_t>
  query(CFGBuilder &Builder, const BinaryImage &Image, va_t Entry,
        size_t Budget = limits::kMaxX87CallProofWork, unsigned Depth = 0) {
    const auto Effect =
        Builder.x87CallEffectForTesting(Image, Entry, Budget, Depth);
    return {Effect, Budget};
  }
};
} // namespace neverd::detail

class X86_32_X87FPU : public NeverDLiftTest {};

static fs::path testObj() {
  return fs::path(TEST_OBJ_DIR) / "test_x87_fpu32.o";
}

TEST_F(X86_32_X87FPU, AllStagesPass) {
  ASSERT_TRUE(fs::exists(testObj())) << "test_x87_fpu32.o not built";
  verifyAllStages(testObj());
}

TEST_F(X86_32_X87FPU, NoUnlifted) { verifyNoUnlifted(testObj()); }

TEST_F(X86_32_X87FPU, NoUnreachable) { verifyLLVMIRNoUnreachable(testObj()); }

TEST_F(X86_32_X87FPU, FaddPreservesSemantics) {
  verifyLowIRContains(testObj(), "test_fadd32", "FLOAT_ADD");
}

TEST_F(X86_32_X87FPU, FsubPreservesSemantics) {
  verifyLowIRContains(testObj(), "test_fsub32", "FLOAT_SUB");
}

TEST_F(X86_32_X87FPU, FmulPreservesSemantics) {
  verifyLowIRContains(testObj(), "test_fmul32", "FLOAT_MULT");
}

TEST_F(X86_32_X87FPU, FdivPreservesSemantics) {
  verifyLowIRContains(testObj(), "test_fdiv32", "FLOAT_DIV");
}

TEST_F(X86_32_X87FPU, FabsPreservesSemantics) {
  verifyLowIRContains(testObj(), "test_fabs32", "INT_AND");
}

TEST_F(X86_32_X87FPU, FchsPreservesSemantics) {
  verifyLowIRContains(testObj(), "test_fchs32", "INT_XOR");
}

TEST_F(X86_32_X87FPU, FildFistpPreservesSemantics) {
  auto r = liftToLowIR(testObj());
  ASSERT_TRUE(r.ok());
  EXPECT_TRUE(r.contains("FLOAT_INT2FLOAT") || r.contains("FLOAT_FLOAT2INT"))
      << "FILD/FISTP should produce float conversion ops";
}

TEST_F(X86_32_X87FPU, DecompileSucceeds) {
  verifyDecompileProducesOutput(testObj());
}

TEST_F(X86_32_X87FPU, LLVMIRNoVerifierErrors) {
  verifyLLVMIRNoVerifierErrors(testObj());
}

TEST_F(X86_32_X87FPU, NoConstantTrueBranch) {
  verifyNoConstantTrueBranch(testObj());
}

TEST_F(X86_32_X87FPU, AllModesSucceed) { verifyAllModesSucceed(testObj()); }

namespace {
using namespace neverd;

BinaryImage x87CallLoopImage(Arch Architecture, BinaryFormat Format,
                             unsigned Mutation = 0, bool Extended = false) {
  BinaryImage Image;
  Image.Arch = Architecture;
  Image.Bits = Architecture == Arch::X86 ? Bitness::Bits32 : Bitness::Bits64;
  Image.Format = Format;
  Image.Entry = 0x1000;
  Segment Text;
  Text.Name = ".text";
  Text.VA = Image.Entry;
  Text.Size = 0x300;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0xcc);
  auto Publish = [&](va_t Address, const char *Name,
                     const std::vector<uint8_t> &Bytes) {
    std::copy(Bytes.begin(), Bytes.end(),
              Text.Data.begin() + Address - Text.VA);
    Symbol Sym = Symbol::makeFunc(Address, Bytes.size());
    Sym.Name = Name;
    Image.Symbols.push_back(Sym);
    Image.KnownCodeRanges.emplace_back(Address, Address + Bytes.size());
  };
  std::vector<uint8_t> Caller;
  auto Append = [&](std::initializer_list<uint8_t> Bytes) {
    Caller.insert(Caller.end(), Bytes);
  };
  if (Architecture == Arch::X64)
    Append({0x48});
  Append({0x83, 0xec, 0x18});             // sub sp,24
  Append({0xc7, 0x04, 0x24, 3, 0, 0, 0}); // remaining = 3
  Append({0xd9, 0xee});                   // sum = 0.0
  if (Extended)
    Append({0xdb, 0x7c, 0x24, 8}); // fstpt
  else
    Append({0xdd, 0x5c, 0x24, 8}); // fstpl
  const size_t Loop = Caller.size();
  const uint32_t Displacement = 0x1100 - (0x1000 + Loop + 5);
  Append({0xe8});
  for (unsigned I = 0; I != 4; ++I)
    Append({uint8_t(Displacement >> (8 * I))});
  Append({0xeb, 0}); // consume in a successor block
  if (Extended)
    Append({0xdb, 0x6c, 0x24, 8}); // fldt sum
  else
    Append({0xdd, 0x44, 0x24, 8}); // fldl sum
  Append({0xde, 0xc1});            // faddp
  if (Extended)
    Append({0xdb, 0x7c, 0x24, 8});
  else
    Append({0xdd, 0x5c, 0x24, 8});
  Append({0x83, 0x2c, 0x24, 1}); // --remaining
  const int Back = int(Loop) - int(Caller.size() + 2);
  Append({0x75, static_cast<uint8_t>(Back)});
  Append({0x8b, 0x44, 0x24, uint8_t(Extended ? 8 : 12)});
  if (Architecture == Arch::X64)
    Append({0x48});
  Append({0x83, 0xc4, 0x18, 0xc3});
  Publish(0x1000, "x87_call_loop", Caller);
  Publish(0x1100, "x87_forwarder", {0xe8, 0xfb, 0, 0, 0, 0xc3});
  std::vector<uint8_t> Leaf{0xd9, 0xe8, 0xc3}; // fld1; ret
  if (Mutation == 1)
    Leaf = {0x85, 0xc0, 0x74, 3, 0xd9, 0xe8, 0xc3, 0x31, 0xc0, 0xc3};
  else if (Mutation == 2)
    Leaf = {0xd9, 0xf6, 0xc3}; // fdecstp does not initialize a returned value
  else if (Mutation == 3)
    Leaf = {0xe8, 0xfb, 0, 0, 0, 0xd9, 0xe8, 0xc3}; // opaque call at 0x1300
  else if (Mutation == 4)
    Leaf = {0xd9, 0xe8, 0xeb, 0xfc}; // nonzero stack change on the loop
  else if (Mutation == 5)
    Leaf = {0xd9, 0xe8, 0xdd, 0xd8, 0xc3}; // balanced, integer result
  else if (Mutation == 6)
    Leaf = {0xd9, 0xe8, 0xdd, 0xc0, 0xc3}; // ffree st0 invalidates its tag
  else if (Mutation == 7)
    Leaf = {0xdb, 0xe3, 0xd9, 0xe8, 0xc3}; // fninit resets incoming TOP
  else if (Mutation == 8)
    Leaf = {0x0f, 0xae, 0x0c, 0x24, 0xd9, 0xe8, 0xc3}; // fxrstor [sp]
  else if (Mutation == 9)
    Leaf = {0xd9, 0xc0, 0xc3}; // fld st0 reads an uninitialized entry slot
  else if (Mutation == 10)
    Leaf = {0xd9, 0xe8, 0xdd, 0xd8, 0xd9, 0xc0, 0xc3}; // read after pop
  else if (Mutation == 11)
    Leaf = {0xcc, 0xd9, 0xe8, 0xc3}; // resumable debug trap is opaque
  else if (Mutation == 12)
    Leaf = {0xe8, 0xfb, 0xfe, 0xff, 0xff, 0xd9, 0xe8, 0xc3}; // recursion
  else if (Mutation == 13)
    Leaf = {0xd9, 0xe8, 0xd9, 0xfe, 0xc3}; // fld1; fsin; ret
  else if (Mutation == 14 || Mutation == 15)
    Leaf = {0xd9, 0xe8, // fld1
            0xb8, 42,
            0,    0,
            0,    0x31,
            0xd2, // edx:eax = 42
            0xb9, 6,
            0,    0,
            0,                                           // ecx = 6
            0xf7, uint8_t(Mutation == 14 ? 0xf1 : 0xf9), // div/idiv ecx
            0xc3};
  if (Extended) {
    Leaf.clear();
    if (Architecture == Arch::X64)
      Leaf.push_back(0x48);
    const std::vector<uint8_t> Body = {
        0x83, 0xec, 0x10, 0xc7, 0x04, 0x24, 8,    0,    0,    0,
        0xc7, 0x44, 0x24, 4,    0,    0,    0,    0x80, 0x66, 0xc7,
        0x44, 0x24, 8,    0xff, 0x3f, 0xdb, 0x2c, 0x24}; // fldt of 1 + 2^-60,
                                                         // beyond double
                                                         // precision
    Leaf.insert(Leaf.end(), Body.begin(), Body.end());
    if (Architecture == Arch::X64)
      Leaf.push_back(0x48);
    Leaf.insert(Leaf.end(), {0x83, 0xc4, 0x10, 0xc3});
  }
  Publish(0x1200, "x87_leaf", Leaf);
  Image.Segments.push_back(std::move(Text));
  return Image;
}

BinaryImage x87ConsumingCallImage(
    BinaryFormat Format,
    const std::vector<uint8_t> &Callee = {0x83, 0xec, 4, 0xdb, 0x1c, 0x24, 0x58,
                                          0xc3},
    bool Extended = false, bool IntegerArgs = false) {
  auto Image = x87CallLoopImage(Arch::X86, Format);
  auto &Text = Image.Segments[0];
  std::fill(Text.Data.begin(), Text.Data.end(), 0xcc);
  std::vector<uint8_t> Caller{0x83, 0xec, 0x10,                // sub esp,16
                              0xc7, 0x04, 0x24, 3, 0, 0, 0,    // remaining = 3
                              0xc7, 0x44, 0x24, 4, 0, 0, 0, 0, // sum = 0
                              0xc7, 0x44, 0x24, 8, 7, 0, 0, 0, // retained = 7
                              0xdb, 0x44, 0x24, 8};            // fild retained
  const size_t Loop = Caller.size();
  Caller.insert(Caller.end(), {0x8b, 0x04, 0x24,       // eax = remaining
                               0x89, 0x44, 0x24, 12,   // local = eax
                               0xdb, 0x44, 0x24, 12}); // fild local
  if (IntegerArgs)
    Caller.insert(Caller.end(),
                  {0x89, 0x04, 0x24, // explicit outgoing stack argument
                   0xb9, 11, 0, 0, 0, 0xba, 23, 0, 0, 0}); // ecx, edx
  const uint32_t Disp = 0x1100 - (0x1000 + Caller.size() + 5);
  Caller.push_back(0xe8);
  for (unsigned I = 0; I < 4; ++I)
    Caller.push_back(uint8_t(Disp >> (8 * I)));
  Caller.insert(Caller.end(), {0x01, 0x44, 0x24, 4, // sum += eax
                               0x83, 0x2c, 0x24, 1});
  const int Back = int(Loop) - int(Caller.size() + 2);
  Caller.insert(Caller.end(),
                {0x75, uint8_t(Back), 0xdb, 0x5c, 0x24, 8, // fistp retained
                 0x8b, 0x44, 0x24, 4,                      // eax = sum
                 0x03, 0x44, 0x24, 8,                      // eax += retained
                 0x83, 0xc4, 0x10, 0xc3});
  if (Extended)
    Caller = {0x83, 0xec, 0x10, 0xc7, 0x04, 0x24, 8,    0,    0,    0,
              0xc7, 0x44, 0x24, 4,    0,    0,    0,    0x80, 0x66, 0xc7,
              0x44, 0x24, 8,    0xff, 0x3f, 0xdb, 0x2c, 0x24, // fldt 1 + 2^-60
              0xe8, 0xdf, 0,    0,    0,    0x83, 0xc4, 0x10, 0xc3};
  std::copy(Caller.begin(), Caller.end(), Text.Data.begin());
  std::copy(Callee.begin(), Callee.end(), Text.Data.begin() + 0x100);
  Image.Symbols.resize(2);
  Image.Symbols[0].Size = Caller.size();
  Image.Symbols[1].Size = Callee.size();
  Image.KnownCodeRanges = {{0x1000, 0x1000 + Caller.size()},
                           {0x1100, 0x1100 + Callee.size()}};
  return Image;
}

void wrapX87Consumer(BinaryImage &Image, bool Tail) {
  auto &Bytes = Image.Segments[0].Data;
  const size_t Size = Image.Symbols[1].Size;
  std::copy_n(Bytes.begin() + 0x100, Size, Bytes.begin() + 0x200);
  std::fill(Bytes.begin() + 0x100, Bytes.begin() + 0x200, 0xcc);
  const std::array<uint8_t, 6> Wrapper{
      uint8_t(Tail ? 0xe9 : 0xe8), 0xfb, 0, 0, 0, 0xc3};
  std::copy(Wrapper.begin(), Wrapper.end(), Bytes.begin() + 0x100);
  Image.Symbols[1].Size = Tail ? 5 : 6;
  Image.KnownCodeRanges[1].second = 0x1100 + Image.Symbols[1].Size;
  auto Symbol = neverd::Symbol::makeFunc(0x1200, Size);
  Symbol.Name = "x87_consumer_leaf";
  Image.Symbols.push_back(Symbol);
  Image.KnownCodeRanges.emplace_back(0x1200, 0x1200 + Size);
}

bool hasX87CallDefinition(const LowFunc &Function) {
  for (const auto &B : Function.Blocks)
    for (const auto &Op : B.Ops)
      if ((Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
          Op.Output.isReg() && Op.Output.Size == x86reg::FPURegSize &&
          Op.Output.Offset >= x86reg::ST0 && Op.Output.Offset <= x86reg::ST7)
        return true;
  return false;
}

// Whether \p Image's calling convention owns the x87 stack as one context.
bool ownsX87(const BinaryImage &Image) {
  return x87ContextOwnsStack(Image.Arch, Image.abiFormat(),
                             Image.X87ContextOverride);
}

void expectOwnedCallTransport(const LowFunc &Function) {
  unsigned Calls = 0, Refreshed = 0, Imports = 0, Exports = 0, Captures = 0;
  for (const auto &B : Function.Blocks)
    for (size_t I = 0; I < B.Ops.size(); ++I) {
      const auto &Op = B.Ops[I];
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
        ++Calls;
        // A call whose RETURN follows directly hands the callee's state
        // straight back; any other call refreshes the context after it.
        if (I + 1 == B.Ops.size() || B.Ops[I + 1].Opcode != NdOp::RETURN)
          ++Refreshed;
        EXPECT_FALSE(Op.Output.isReg() && Op.Output.Size == 10);
        ASSERT_GT(I, 0u);
        const auto &Before = B.Ops[I - 1];
        EXPECT_EQ(Before.Opcode, NdOp::INTRINSIC);
        ASSERT_EQ(Before.NumInputs, 2u);
        EXPECT_EQ(Before.Inputs[0].Offset,
                  unsigned(Intrinsic::X87ContextExport));
      }
      if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
          Op.Inputs[0].isConst()) {
        const auto Id = Intrinsic(Op.Inputs[0].Offset);
        Captures += Id == Intrinsic::X87ContextBegin;
        Imports += Id == Intrinsic::X87ContextRefresh;
        Exports += Id == Intrinsic::X87ContextExport;
        if (isX87ContextIntrinsic(Id))
          EXPECT_TRUE(x87ContextLowShapeIsValid(Op, Arch::X64));
      }
    }
  EXPECT_GT(Captures, 0u);
  EXPECT_GE(Imports, Refreshed);
  EXPECT_GE(Exports, Calls);
}

BinaryImage x87ExamineImage(Arch A, BinaryFormat Format,
                            const std::vector<uint8_t> &Bytes) {
  BinaryImage Image;
  Image.Arch = A;
  Image.Format = Format;
  Image.Bits = A == Arch::X86 ? Bitness::Bits32 : Bitness::Bits64;
  Image.Entry = 0x1000;
  Segment Text;
  Text.Name = ".text";
  Text.VA = Image.Entry;
  Text.Data = Bytes;
  Text.Size = Text.FileSz = Bytes.size();
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Image.Segments.push_back(Text);
  Symbol Sym = Symbol::makeFunc(Image.Entry, Bytes.size());
  Sym.Name = "examine_value";
  Image.Symbols.push_back(Sym);
  Image.KnownCodeRanges.emplace_back(Image.Entry, Image.Entry + Bytes.size());
  return Image;
}

BinaryImage x87SwitchImage(Arch A) {
  // A linked PE switch with relocated pointers embedded after the code.
  // All ordinary paths enter with an empty x87 stack and define ST0.
  std::vector<uint8_t> Bytes{0x8b, 0x44, 0x24,
                             4, // mov eax, [sp+4]
                             0x83, 0xf8, 2,
                             0x77, 0x19, // cmp eax, 2; ja 1022
                             0xff, 0x24, uint8_t(A == Arch::X86 ? 0x85 : 0xc5),
                             0x40, 0x10, 0,
                             0, // jmp [ax*ptrsize+1040]
                             0xdb, 0x44, 0x24,
                             8,    0xeb, 0x0e, // 1010: fild; jmp 1024
                             0xdb, 0x44, 0x24,
                             12,   0xeb, 8, // 1016: fild; jmp 1024
                             0xdb, 0x44, 0x24,
                             16,   0xeb, 2, // 101c: fild; jmp 1024
                             0xd9, 0xee,    // 1022: fldz
                             0xd9, 0xe5, 0xdf,
                             0xe0, 0xdd, 0xd8, // fxam; fnstsw ax; pop
                             0x25, 0,    0x47,
                             0,    0,    0xc3}; // and eax, 4700; ret
  auto Image = x87ExamineImage(A, BinaryFormat::COFF, Bytes);
  Image.IsRelocatable = false;
  auto &Text = Image.Segments.front();
  Text.Data.resize(0x40, 0xcc);
  std::vector<AbsolutePointerRelocation> Relocations;
  for (va_t Target : {0x1010, 0x1016, 0x101c}) {
    const va_t Slot = Text.VA + Text.Data.size();
    for (unsigned I = 0; I < Image.getPointerSize(); ++I)
      Text.Data.push_back(uint8_t(Target >> (8 * I)));
    Relocations.push_back({Slot, Target, InvalidVA});
  }
  Text.Size = Text.FileSz = Text.Data.size();
  if (A == Arch::X86)
    Relocations.push_back({0x100c, 0x1040, InvalidVA});
  recordAbsolutePointerRelocations(Image, Relocations);
  coff_loader::recordRelocatedPointerRunsInCode(Image);
  if (A == Arch::X64) {
    Image.DataAddressRelocOperands[0x100c] = {0x1040, 0x1040, 4, 0x1000};
    Image.RelocDataAddrs.insert(0x1040);
  }
  return Image;
}

BinaryImage x87CRTCallImage(const char *Name, unsigned Inputs,
                            unsigned Route = 0) {
  std::vector<uint8_t> Bytes;
  for (unsigned I = 0; I < Inputs; ++I)
    Bytes.insert(Bytes.end(), {0xd9, 0x44, 0x24, uint8_t(4 + 4 * I)});
  if (Route == 0) {
    const uint32_t Displacement = 0x1100 - (0x1000 + Bytes.size() + 5);
    Bytes.push_back(0xe8);
    for (unsigned K = 0; K < 4; ++K)
      Bytes.push_back(uint8_t(Displacement >> (8 * K)));
  } else if (Route == 1) {
    Bytes.insert(Bytes.end(), {0xff, 0x15, 0, 0x20, 0, 0}); // call [IAT]
  } else {
    Bytes.insert(Bytes.end(), {0x8b, 0x0d, 0, 0x20, 0, 0, 0xff, 0xd1});
  }
  Bytes.push_back(0xc3);
  auto Image = x87ExamineImage(Arch::X86, BinaryFormat::COFF, Bytes);
  auto &Text = Image.Segments[0];
  Text.Data.resize(0x106, 0xcc);
  const std::array<uint8_t, 6> Stub{0xff, 0x25, 0, 0x20, 0, 0};
  std::copy(Stub.begin(), Stub.end(), Text.Data.begin() + 0x100);
  Text.Size = Text.FileSz = Text.Data.size();
  Segment Slot;
  Slot.Name = ".idata";
  Slot.VA = 0x2000;
  Slot.Size = Slot.FileSz = 4;
  Slot.Data.resize(4);
  Slot.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Image.Segments.push_back(std::move(Slot));
  Import Imported;
  Imported.Name = Name;
  Imported.Module = "MSVCR100.dll";
  Imported.IATAddr = 0x2000;
  Image.Imports.push_back(std::move(Imported));
  EXPECT_TRUE(Image.recordImportStorageSlot(
      0x2000, Name, 0, ImportStorageEvidence::ImportDirectory));
  Image.recordImportStub(0x1100, 0);
  return Image;
}

TEST(X87ImportCall, NativeCallsCarryTheOriginalStackArgumentsAndResult) {
  for (const char *Name :
       {"_CIsqrt", "_CIsin", "_CIcos", "_CIatan", "_CIacos", "_CIpow"})
    for (unsigned Route = 0; Route < 3; ++Route)
      for (bool NoOpt : {false, true}) {
        SCOPED_TRACE(Name);
        SCOPED_TRACE(Route);
        SCOPED_TRACE(NoOpt);
        const unsigned Count = std::string(Name) == "_CIpow" ? 2 : 1;
        auto Image = x87CRTCallImage(Name, Count, Route);
        llvm::LLVMContext Context;
        PipelineOptions Options;
        Options.EmitDumpOutput = false;
        Options.NoOpt = NoOpt;
        Options.LiftMode = true;
        Options.OnlyFunctionEntries = {0x1000};
        auto Result = Pipeline().run(Image, Context, Options);
        ASSERT_TRUE(Result.Success) << Result.Error;
        ASSERT_TRUE(Result.LlvmModule);
        ASSERT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
        auto *Runtime = Result.LlvmModule->getFunction(Name);
        ASSERT_TRUE(Runtime);
        EXPECT_TRUE(Runtime->isDeclaration());
        EXPECT_TRUE(Runtime->arg_empty());
        unsigned Adapters = 0, Calls = 0;
        for (auto &Function : *Result.LlvmModule)
          for (auto &Block : Function)
            for (auto &Instruction : Block)
              if (auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction)) {
                if (Call->isInlineAsm()) {
                  const auto *Assembly =
                      llvm::cast<llvm::InlineAsm>(Call->getCalledOperand());
                  if (!Assembly->getAsmString().starts_with("calll *"))
                    continue;
                  ++Adapters;
                  EXPECT_TRUE(Assembly->canThrow());
                  ASSERT_EQ(Call->arg_size(), Count + 1);
                  EXPECT_EQ(Call->getArgOperand(Count), Function.getArg(Count));
                  for (unsigned K = 0; K < Count; ++K)
                    EXPECT_TRUE(
                        Call->getArgOperand(K)->getType()->isX86_FP80Ty());
                  EXPECT_TRUE(Call->getType()->isX86_FP80Ty());
                } else if (auto *Target = Call->getCalledFunction();
                           Target && Target->hasFnAttribute(
                                         "neverd.x87-import-adapter")) {
                  ++Calls;
                  EXPECT_EQ(Call->arg_size(), Count + 1);
                  EXPECT_TRUE(
                      Call->getArgOperand(Count)->getType()->isPointerTy());
                  if (Route == 0)
                    EXPECT_EQ(Call->getArgOperand(Count), Runtime);
                  EXPECT_TRUE(Call->getType()->isX86_FP80Ty());
                  EXPECT_FALSE(Target->doesNotThrow());
                }
              }
        EXPECT_EQ(Adapters, 1u);
        EXPECT_EQ(Calls, 1u);
        CEmitterOptions Emit;
        Emit.TheArch = Arch::X86;
        Emit.Format = BinaryFormat::COFF;
        std::string C;
        llvm::raw_string_ostream COS(C);
        EXPECT_THROW(
            LLVMCEmitter().emit(*Result.LlvmModule, COS, Emit, nullptr, &Image),
            std::runtime_error);
        EXPECT_THROW(LLVMCEmitter().emit(
                         *Result.LlvmModule, COS, Emit, nullptr, &Image,
                         Result.LlvmModule->getFunction("examine_value")),
                     std::runtime_error);
        EXPECT_TRUE(C.empty());
        ASSERT_FALSE(Result.MedFuncs.empty());
        EXPECT_THROW(
            MedToHighConverter::validateCallContracts(Result.MedFuncs.front()),
            std::runtime_error);
        Options.LiftMode = false;
        Options.PatchMode = true;
        auto Patch = Pipeline().run(Image, Context, Options);
        ASSERT_TRUE(Patch.Success) << Patch.Error;
        ASSERT_TRUE(Patch.LlvmModule);
        EXPECT_FALSE(llvm::verifyModule(*Patch.LlvmModule, &llvm::errs()));
        Options.PatchMode = false;
        auto Source = Pipeline().run(Image, Context, Options);
        ASSERT_TRUE(Source.Success) << Source.Error;
        ASSERT_FALSE(Source.HighFuncs.empty());
        EXPECT_TRUE(Source.HighFuncs.front().AnalysisOnly);
        std::string AnalysisC;
        llvm::raw_string_ostream AnalysisOS(AnalysisC);
        ASSERT_TRUE(HighCEmitter().emit(Source.HighFuncs, AnalysisOS, Emit));
        EXPECT_NE(AnalysisC.find("neverd_analysis_x87_call"),
                  std::string::npos);
        Options.DeferHighIR = true;
        auto Deferred = Pipeline().run(Image, Context, Options);
        ASSERT_TRUE(Deferred.Success) << Deferred.Error;
        EXPECT_TRUE(Deferred.HighIRDeferred);
        EXPECT_TRUE(Pipeline().resumeHighIR(Image, Options, Deferred));
        ASSERT_FALSE(Deferred.HighFuncs.empty());
        EXPECT_TRUE(Deferred.HighFuncs.front().AnalysisOnly);
      }
}

TEST(X87ImportCall, UnprovedInputsAndUnsupportedNativeContractsStayRejected) {
  for (unsigned Mutation = 0; Mutation < 11; ++Mutation) {
    auto Image = x87CRTCallImage(Mutation == 2 ? "_CIpow" : "_CIsqrt",
                                 Mutation == 0   ? 0
                                 : Mutation == 1 ? 2
                                                 : 1);
    if (Mutation == 3)
      Image.Imports[0].Module = "unrelated.dll";
    if (Mutation == 4)
      Image.Imports[0].IsDelayImport = true;
    if (Mutation == 5)
      Image.Imports[0].Name = "_CItan"; // not yet modeled
    if (Mutation == 6)
      Image.Format = BinaryFormat::ELF;
    if (Mutation == 7) {
      Image.Arch = Arch::X64;
      Image.Bits = Bitness::Bits64;
      // In long mode the stub uses RIP-relative rather than absolute memory.
      const uint32_t Displacement = 0x2000 - 0x1106;
      for (unsigned K = 0; K < 4; ++K)
        Image.Segments[0].Data[0x102 + K] = uint8_t(Displacement >> (8 * K));
      Image.Segments[1].Size = Image.Segments[1].FileSz = 8;
      Image.Segments[1].Data.resize(8);
    }
    if (Mutation == 8) {
      auto Other = Image.Imports[0];
      Other.Module = "unrelated.dll";
      Image.Imports.push_back(std::move(Other));
    }
    if (Mutation == 9)
      Image.ImportStorageSlots.clear();
    if (Mutation == 10) {
      auto Other = Image.Imports[0];
      Other.IATAddr += 4;
      Other.Module = "msvcrt.dll";
      Image.Segments[1].Size = Image.Segments[1].FileSz = 8;
      Image.Segments[1].Data.resize(8);
      ASSERT_TRUE(Image.recordImportStorageSlot(
          Other.IATAddr, Other.Name, 0,
          ImportStorageEvidence::ImportDirectory));
      Image.Imports.push_back(std::move(Other));
    }
    SCOPED_TRACE(Mutation);
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.LiftMode = true;
    Options.OnlyFunctionEntries = {0x1000};
    auto Result = Pipeline().run(Image, Context, Options);
    EXPECT_FALSE(Result.Success);
    EXPECT_FALSE(Result.LlvmModule);
    ASSERT_EQ(Result.FunctionAudits.size(), 1u);
    EXPECT_EQ(Result.FunctionAudits[0].Disposition,
              PipelineFunctionDisposition::RejectedIncomplete);
  }
  struct NoReturn final : NoReturnCalleeProver {
    bool mayNeverReturn(va_t) const override { return true; }
    bool neverReturns(va_t, unsigned) const override { return true; }
  } Proof;
  auto Image = x87CRTCallImage("_CIsqrt", 1);
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Image));
  CFGBuilder Builder;
  Builder.setNoReturnCalleeProver(&Proof);
  const auto Low = Builder.build(Image, Dec, Image.Entry);
  EXPECT_FALSE(Low.hasCompleteInstructionLift());
}

TEST(X87ImportCall, AdapterDeclarationsRequireTheCheckedPE32ABI) {
  for (unsigned Mutation = 0; Mutation < 7; ++Mutation) {
    llvm::LLVMContext Context;
    llvm::Module Module("invalid-crt-adapter", Context);
    Module.setTargetTriple(llvm::Triple(Mutation == 0 ? "i386-linux-gnu"
                                        : Mutation == 1
                                            ? "x86_64-pc-windows-msvc"
                                            : "i686-pc-windows-msvc"));
    auto *FP = llvm::Type::getX86_FP80Ty(Context);
    auto *Type = llvm::FunctionType::get(FP, false);
    if (Mutation == 2)
      Type = llvm::FunctionType::get(FP, {FP}, false);
    auto *Runtime = llvm::Function::Create(
        Type, llvm::GlobalValue::ExternalLinkage, "_CIsqrt", Module);
    if (Mutation == 3)
      Runtime->setCallingConv(llvm::CallingConv::X86_StdCall);
    if (Mutation == 4)
      Runtime->addFnAttr(llvm::Attribute::NoUnwind);
    if (Mutation == 6)
      new llvm::GlobalVariable(Module, llvm::Type::getInt8Ty(Context), false,
                               llvm::GlobalValue::ExternalLinkage, nullptr,
                               "__nd_x87_import__CIsqrt");
    auto Adapter = createX87ImportCallAdapter(*Runtime, Mutation == 5 ? 3 : 1);
    EXPECT_FALSE(bool(Adapter)) << Mutation;
    if (!Adapter)
      llvm::consumeError(Adapter.takeError());
  }
}

TEST_F(X86_32_X87FPU, NativeCRTStackCallsMatchIndependentMachineCallers) {
#if !defined(__linux__) || !defined(__x86_64__)
  GTEST_SKIP() << "requires a Linux x86-64 host with native i386 execution";
#else
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "requires Clang's i386 target";
  for (unsigned Count : {1u, 2u})
    for (unsigned Route = 0; Route < 3; ++Route)
      for (bool NoOpt : {false, true}) {
        SCOPED_TRACE(Count);
        SCOPED_TRACE(Route);
        SCOPED_TRACE(NoOpt);
        const char *Name = Count == 1 ? "_CIsqrt" : "_CIpow";
        auto Image = x87CRTCallImage(Name, Count, Route);
        llvm::LLVMContext Context;
        PipelineOptions Options;
        Options.EmitDumpOutput = false;
        Options.LiftMode = true;
        Options.NoOpt = NoOpt;
        Options.OnlyFunctionEntries = {0x1000};
        auto Result = Pipeline().run(Image, Context, Options);
        ASSERT_TRUE(Result.Success) << Result.Error;
        ASSERT_TRUE(Result.LlvmModule);
        ASSERT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
        std::string IR;
        llvm::raw_string_ostream Stream(IR);
        neverd::test::printHostCompilerFixture(*Result.LlvmModule, Stream);
        const auto LLVMFile = tmpFile("crt-stack.ll");
        std::ofstream(LLVMFile) << IR;
        const auto NativeFile = tmpFile("crt-stack-native.s");
        std::ofstream(NativeFile)
            << ".text\n.globl examine_value\nexamine_value:\nflds 4(%esp)\n"
            << (Count == 2 ? "flds 8(%esp)\n" : "") << "call " << Name
            << "\nret\n.section .note.GNU-stack,\"\",@progbits\n";
        const auto HarnessFile = tmpFile("crt-stack-start.s");
        std::ofstream(HarnessFile)
            << ".text\n.globl " << Name << "\n"
            << Name
            << ":\n"
            // Deliberately different from sqrt/pow: replacing the imported
            // routine by a mathematical operation must fail this oracle.
            << (Count == 1 ? "fchs\n" : ".byte 0xde,0xe9\n")
            << "movl $17,%eax\nmovl $19,%ecx\nmovl $23,%edx\nret\n"
               ".globl _start\n_start:\nfninit\npushl $0x40400000\n"
               "pushl $0x40a00000\ncall examine_value\naddl $8,%esp\n"
               "flds expected\nfucomip %st(1),%st\nsetne %bl\nsetp %cl\n"
               "orb %cl,%bl\nfstp %st(0)\nmovzbl %bl,%ebx\n"
               "movl $1,%eax\nint $0x80\n.data\nexpected:\n.long "
            << (Count == 1 ? "0xc0a00000" : "0x40000000")
            << "\n.section .note.GNU-stack,\"\",@progbits\n";
        for (const std::string Opt : {"-O0", "-O2"}) {
          const auto Object = tmpFile("crt-stack.obj");
          const auto COFF = exec(NEVERD_TEST_CLANG,
                                 {"--target=i686-pc-windows-msvc", Opt, "-c",
                                  LLVMFile.string(), "-o", Object.string()});
          ASSERT_TRUE(COFF.ok()) << COFF.err << IR;
          for (const auto &Source : {NativeFile, LLVMFile}) {
            const auto Binary = tmpFile("crt-stack-execution");
            const auto Build = exec(
                NEVERD_TEST_CLANG,
                {"--target=i386-linux-gnu", Opt, "-ffreestanding",
                 "-fuse-ld=lld", "-nostdlib", "-static", "-no-pie",
                 Source.string(), HarnessFile.string(), "-o", Binary.string()});
            ASSERT_TRUE(Build.ok()) << Build.err << IR;
            const auto Run = exec(Binary.string(), {});
            ASSERT_TRUE(Run.ok()) << Run.err << IR;
          }
        }
      }
#endif
}

TEST_F(X86_32_X87FPU, NativeCRTAdaptersTransportAllBinary80Bits) {
#if !defined(__linux__) || !defined(__x86_64__)
  GTEST_SKIP() << "requires a Linux x86-64 host with native i386 execution";
#else
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "requires Clang's i386 target";
  llvm::LLVMContext Context;
  llvm::Module Module("x87-crt-adapter", Context);
  Module.setTargetTriple(llvm::Triple("i686-pc-windows-msvc"));
  auto *FP = llvm::Type::getX86_FP80Ty(Context);
  auto *Bits = llvm::Type::getIntNTy(Context, 80);
  auto *Check = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getInt32Ty(Context), false),
      llvm::GlobalValue::ExternalLinkage, "check_adapter", Module);
  llvm::IRBuilder<> Builder(llvm::BasicBlock::Create(Context, "entry", Check));
  auto rawBits = [&](uint64_t Mantissa, uint64_t Exponent) {
    const uint64_t Words[] = {Mantissa, Exponent};
    return llvm::ConstantInt::get(Context, llvm::APInt(80, Words));
  };
  llvm::Value *Failed = Builder.getFalse();
  for (unsigned Count : {1u, 2u}) {
    auto *Runtime = llvm::Function::Create(
        llvm::FunctionType::get(FP, false), llvm::GlobalValue::ExternalLinkage,
        Count == 1 ? "crt_unary" : "crt_binary", Module);
    auto Adapter = createX87ImportCallAdapter(*Runtime, Count);
    ASSERT_TRUE(bool(Adapter)) << llvm::toString(Adapter.takeError());
    llvm::SmallVector<llvm::Value *, 2> Args{Builder.CreateBitCast(
        rawBits(UINT64_C(0x8000000000000008), 0x3fff), FP)};
    if (Count == 2)
      Args.push_back(Builder.CreateBitCast(
          rawBits(UINT64_C(0xc000000000000000), 0x4000), FP));
    Args.push_back(Runtime);
    auto *Result =
        Builder.CreateBitCast(Builder.CreateCall(*Adapter, Args), Bits);
    auto *Expected = Count == 1 ? rawBits(UINT64_C(0x8000000000000008), 0xbfff)
                                : rawBits(UINT64_C(0xfffffffffffffff8), 0x3fff);
    Failed = Builder.CreateOr(Failed, Builder.CreateICmpNE(Result, Expected));
  }
  Builder.CreateRet(Builder.CreateZExt(Failed, Builder.getInt32Ty()));
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string IR;
  llvm::raw_string_ostream Stream(IR);
  neverd::test::printHostCompilerFixture(Module, Stream);
  const auto LLVMFile = tmpFile("crt-adapter-binary80.ll");
  std::ofstream(LLVMFile) << IR;
  const auto HarnessFile = tmpFile("crt-adapter-start.s");
  std::ofstream(HarnessFile)
      << ".text\n.globl crt_unary\ncrt_unary:\nfchs\nret\n"
         ".globl crt_binary\ncrt_binary:\n.byte 0xde,0xe9\nret\n"
         ".globl _start\n_start:\nfninit\ncall check_adapter\n"
         "movl %eax,%ebx\nmovl $1,%eax\nint $0x80\n"
         ".section .note.GNU-stack,\"\",@progbits\n";
  for (const std::string Opt : {"-O0", "-O2"}) {
    const auto Binary = tmpFile("crt-adapter-execution");
    const auto Build =
        exec(NEVERD_TEST_CLANG,
             {"--target=i386-linux-gnu", Opt, "-ffreestanding", "-fuse-ld=lld",
              "-nostdlib", "-static", "-no-pie", LLVMFile.string(),
              HarnessFile.string(), "-o", Binary.string()});
    ASSERT_TRUE(Build.ok()) << Build.err << IR;
    ASSERT_TRUE(exec(Binary.string(), {}).ok()) << IR;
    const auto Object = tmpFile("crt-adapter.obj");
    const auto COFF =
        exec(NEVERD_TEST_CLANG, {"--target=i686-pc-windows-msvc", Opt, "-c",
                                 LLVMFile.string(), "-o", Object.string()});
    ASSERT_TRUE(COFF.ok()) << COFF.err << IR;
  }
#endif
}

TEST(X87Examine, TagProofSurvivesBranchesPopsAndExplicitInvalidation) {
  const std::vector<std::vector<uint8_t>> Cases{
      {0xd9, 0xe8, 0xd9, 0xe5},             // fld1; fxam
      {0xd9, 0xe5},                         // unknown entry tag
      {0xd9, 0xe8, 0xdd, 0xc0, 0xd9, 0xe5}, // ffree st0; empty
      {0xd9, 0xe8, 0xeb, 0, 0xd9, 0xe5},    // successor inherits tag
      {0xd9, 0xe8, 0xd9, 0xc0, 0xdd, 0xd8, 0xd9, 0xe5}, // dup/pop retains first
      {0xdb, 0xe3, 0xd9, 0xe5},                         // fninit; empty
      {0xd9, 0xe8, 0xe8, 0, 1, 0, 0, 0xd9, 0xe5},       // opaque call
      {0xd9, 0xe8, 0xd9, 0xfe, 0xd9, 0xe5}, // fsin defines full slot
      {0xd9, 0xe8, 0x85, 0xc0, 0x74, 2, 0xdd, 0xc0, 0xd9,
       0xe5},                             // full/empty join
      {0xd9, 0xe8, 0xeb, 0, 0xd9, 0xe5}}; // independent entry
  for (Arch A : {Arch::X86, Arch::X64})
    for (BinaryFormat Format :
         {BinaryFormat::ELF, BinaryFormat::MachO, BinaryFormat::COFF})
      for (unsigned I = 0; I < Cases.size(); ++I) {
        SCOPED_TRACE(static_cast<int>(A));
        SCOPED_TRACE(static_cast<int>(Format));
        SCOPED_TRACE(I);
        auto Bytes = Cases[I];
        Bytes.insert(Bytes.end(), {0xdf, 0xe0, 0x25, 0, 0x47, 0, 0, 0xc3});
        auto Image = x87ExamineImage(A, Format, Bytes);
        if (I == 9)
          Image.CodeRefTargets.insert(0x1004);
        Decoder Dec;
        ASSERT_TRUE(Dec.init(Image));
        CFGBuilder Builder;
        auto Low = Builder.build(Image, Dec, Image.Entry, "examine_value");
        const bool Owned = ownsX87(Image);
        const bool Supported =
            Owned || (I != 1 && I != 5 && I != 6 && I != 8 && I != 9);
        EXPECT_EQ(Low.hasCompleteInstructionLift(), Supported);
        unsigned Pending = 0;
        for (const auto &B : Low.Blocks)
          for (const auto &Op : B.Ops)
            if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
                Op.Inputs[0].isConst() &&
                Op.Inputs[0].Offset ==
                    static_cast<uint64_t>(Intrinsic::X87Fxam))
              ++Pending;
        EXPECT_EQ(Pending == 0, Supported);
        if (Owned) {
          unsigned Examine = 0;
          for (const auto &B : Low.Blocks)
            for (const auto &Op : B.Ops)
              Examine +=
                  Op.Opcode == NdOp::INTRINSIC && Op.NumInputs == 6 &&
                  Op.Inputs[0].Offset == unsigned(Intrinsic::X87ContextStep) &&
                  Op.Inputs[2].Offset == 0xe5d9;
          EXPECT_EQ(Examine, 1u);
        }
      }
}

TEST(X87Examine, OwnedSwitchLabelsInheritStateButIndependentRootsDoNot) {
  for (Arch A : {Arch::X86, Arch::X64})
    for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
      SCOPED_TRACE(static_cast<int>(A));
      SCOPED_TRACE(Mutation);
      auto Image = x87SwitchImage(A);
      ASSERT_TRUE(Image.CodeRefTargets.count(0x1010));
      ASSERT_TRUE(Image.CodePtrRelocSlots.count(0x1040));
      std::set<va_t> Protected;
      if (Mutation == 1) {
        // A separate relocated pointer keeps the label independently live.
        Segment Data;
        Data.Name = ".rdata";
        Data.VA = 0x2000;
        Data.Size = Data.FileSz = Image.getPointerSize();
        Data.Flags = SegmentFlags::Readable;
        Data.Data.resize(Data.Size);
        Data.Data[0] = Data.Data[1] = 0x10;
        Image.Segments.push_back(std::move(Data));
        recordAbsolutePointerRelocation(Image, 0x2000, 0x1010);
      } else if (Mutation == 2) {
        Protected.insert(0x1040);
      } else if (Mutation == 3) {
        // A bare target union without exact slot ownership cannot be erased.
        Image.CodeRefTargets.insert(0x1024);
      } else if (Mutation == 4) {
        // Another reachable instruction takes this same case's address.
        auto &Text = Image.Segments.front();
        Text.Data.insert(Text.Data.begin(), 16, 0x90);
        Text.VA -= 16;
        Text.Size = Text.FileSz = Text.Data.size();
        const std::vector<uint8_t> Prefix =
            A == Arch::X86
                ? std::vector<uint8_t>{0xba, 0x10, 0x10, 0, 0, 0xeb, 9}
                : std::vector<uint8_t>{0x48, 0x8d, 0x15, 0x19, 0,
                                       0,    0,    0xeb, 7};
        std::copy(Prefix.begin(), Prefix.end(), Text.Data.begin());
        Image.Entry = Text.VA;
        Image.Symbols.front().Addr = Image.Entry;
        Image.Symbols.front().Size += 16;
        Image.KnownCodeRanges.front().first = Image.Entry;
        for (auto &[Slot, Field] : Image.DataAddressRelocOperands)
          Field.TargetOwnerVA = Text.VA;
        for (auto &[Slot, Field] : Image.CodeAddressRelocOperands)
          Field.TargetOwnerVA = Text.VA;
        if (A == Arch::X86)
          Image.CodeAddressRelocOperands[Image.Entry + 1] = {0x1010, 0x1010, 4,
                                                             Text.VA};
      } else if (Mutation == 5) {
        Image.CodeRefTargets.insert(0x1014);
      } else if (Mutation == 6) {
        ExceptionFunction EH;
        EH.CodeRange = {Image.Entry, 0x1030};
        EH.Encoding = ExceptionEncoding::DwarfFDE;
        EH.Personality = ExceptionPersonality::GxxPersonalityV0;
        EH.Itanium.emplace();
        ItaniumCallSite Site;
        Site.GuardedRange = {Image.Entry, Image.Entry + 4};
        Site.LandingPadVA = 0x1010;
        EH.Itanium->CallSites.push_back(Site);
        Image.ExceptionMetadata.Functions.push_back(std::move(EH));
        Image.ExceptionMetadata.rebuildIndex();
      }
      Decoder Dec;
      ASSERT_TRUE(Dec.init(Image));
      CFGBuilder Builder;
      const std::set<va_t> Entries{Image.Entry};
      Builder.setKnownFuncEntries(&Entries);
      Builder.setProtectedJumpTableRelocationSlots(&Protected);
      const auto Low = Builder.build(Image, Dec, Image.Entry, "x87_switch");
      std::string Dump;
      llvm::raw_string_ostream Stream(Dump);
      Pipeline::dumpLowIR({Low}, Stream);
      ASSERT_EQ(Low.JumpTables.size(), 1u) << Dump;
      EXPECT_EQ(Low.hasCompleteInstructionLift(),
                A == Arch::X64 || Mutation == 0)
          << Dump;
      if (Mutation == 0 || Mutation == 4)
        EXPECT_EQ(Low.ModuleAnalysisRoots.count(0x1010),
                  Mutation == 4 && A == Arch::X64);
      if (A == Arch::X64) {
        unsigned Captures = 0;
        for (const auto &B : Low.Blocks)
          for (const auto &Op : B.Ops)
            Captures +=
                Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
                Op.Inputs[0].isConst() &&
                Op.Inputs[0].Offset == unsigned(Intrinsic::X87ContextBegin);
        EXPECT_EQ(Captures, Mutation == 0 ? 1u : 2u);
      }
    }
}

TEST_F(X86_32_X87FPU, RelocatedSwitchMatchesNativeIntegerLoadClassification) {
#if !defined(__linux__) || !defined(__x86_64__)
  GTEST_SKIP() << "requires Linux x86-64 and native i386 execution";
#else
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "requires Clang's x86 targets";
  const auto NativePath = tmpFile("x87-switch-native.s");
  std::ofstream(NativePath) << R"(
.intel_syntax noprefix
.text
.globl native_examine
native_examine:
  mov eax, [esp+4]
  cmp eax, 2
  ja .Ldefault
  jmp dword ptr [.Ltable+eax*4]
.Lfirst:
  fild dword ptr [esp+8]
  jmp .Lexamine
.Lsecond:
  fild dword ptr [esp+12]
  jmp .Lexamine
.Lthird:
  fild dword ptr [esp+16]
  jmp .Lexamine
.Ldefault:
  fldz
.Lexamine:
  fxam
  fnstsw ax
  fstp st(0)
  and eax, 0x4700
  ret
.Ltable:
  .long .Lfirst, .Lsecond, .Lthird
.globl _start
_start:
  fninit
  call test_main
  mov ebx, eax
  mov eax, 1
  int 0x80
.section .note.GNU-stack,"",@progbits
)";
  const auto HarnessPath = tmpFile("x87-switch-harness.c");
  std::ofstream(HarnessPath) << R"(
extern unsigned native_examine(unsigned, int, int, int);
extern unsigned examine_value(unsigned, int, int, int);
int test_main(void) {
  static const int values[] = {0, 1, -1, 2147483647, (-2147483647-1), 16777217};
  static const unsigned selectors[] = {0, 1, 2, 3, 0xffffffffu};
  for (unsigned s = 0; s != 5; ++s)
    for (unsigned i = 0; i != 6; ++i) {
      const int a = values[i], b = values[(i+1)%6], c = values[(i+2)%6];
      const unsigned expected = native_examine(selectors[s], a, b, c);
      if (examine_value(selectors[s], a, b, c) != expected) return 1+s*6+i;
    }
  return 0;
}
)";
  for (bool NoOpt : {false, true}) {
    SCOPED_TRACE(NoOpt);
    auto Image = x87SwitchImage(Arch::X86);
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.LiftMode = true;
    Options.NoOpt = NoOpt;
    Options.OnlyFunctionEntries = {Image.Entry};
    auto Result = Pipeline().run(Image, Context, Options);
    ASSERT_TRUE(Result.Success) << Result.Error;
    ASSERT_TRUE(Result.LlvmModule);
    ASSERT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
    std::string IR;
    llvm::raw_string_ostream Stream(IR);
    neverd::test::printHostCompilerFixture(*Result.LlvmModule, Stream);
    const auto IRPath = tmpFile("x87-switch.ll");
    std::ofstream(IRPath) << IR;
    for (const std::string Opt : {"-O0", "-O2"}) {
      SCOPED_TRACE(Opt);
      const auto Binary = tmpFile("x87-switch-exec");
      const auto Build = exec(
          NEVERD_TEST_CLANG,
          {"--target=i386-linux-gnu", Opt, "-msse2", "-ffreestanding",
           "-fuse-ld=lld", "-nostdlib", "-static", "-no-pie", IRPath.string(),
           HarnessPath.string(), NativePath.string(), "-o", Binary.string()});
      ASSERT_TRUE(Build.ok()) << Build.err << IR;
      const auto Run = exec(Binary.string(), {});
      EXPECT_EQ(Run.exitCode, 0) << Run.err << IR;
      const auto Object = tmpFile("x87-switch.obj");
      const auto COFF = exec(NEVERD_TEST_CLANG,
                             {"--target=i686-pc-windows-msvc", Opt, "-msse2",
                              "-c", IRPath.string(), "-o", Object.string()});
      EXPECT_TRUE(COFF.ok()) << COFF.err << IR;
    }
  }
#endif
}

TEST_F(X86_32_X87FPU, ExamineClassificationMatchesNativeExtendedEncodings) {
#if !defined(__linux__) || !defined(__x86_64__)
  GTEST_SKIP() << "requires Linux x86-64 and native i386 execution";
#else
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "requires Clang's x86 targets";
  for (bool Empty : {false, true})
    for (Arch A : {Arch::X86, Arch::X64}) {
      SCOPED_TRACE(static_cast<int>(A));
      SCOPED_TRACE(Empty);
      const std::string Target = A == Arch::X86 ? "--target=i386-linux-gnu"
                                                : "--target=x86_64-linux-gnu";
      std::vector<uint8_t> Bytes =
          A == Arch::X86 ? std::vector<uint8_t>{0x8b, 0x44, 0x24, 4, 0xdb, 0x28}
                         : std::vector<uint8_t>{0xdb, 0x2f};
      // Empty slots retain the payload's sign, even after FFREE. Both paths
      // restore TOP so successive native calls do not overflow the x87 stack.
      if (Empty)
        Bytes.insert(Bytes.end(), {0xdd, 0xc0});           // ffree st0
      Bytes.insert(Bytes.end(), {0xd9, 0xe5, 0xdf, 0xe0}); // fxam; fnstsw ax
      if (Empty)
        Bytes.insert(Bytes.end(), {0xd9, 0xf7}); // fincstp
      else
        Bytes.insert(Bytes.end(), {0xdd, 0xd8}); // fstp st0
      Bytes.insert(Bytes.end(), {0x25, 0, 0x47, 0, 0, 0xc3});
      auto Image = x87ExamineImage(A, BinaryFormat::ELF, Bytes);
      const auto NativePath = tmpFile("examine-native.s");
      std::ofstream Native(NativePath);
      Native << ".text\n.globl native_examine\nnative_examine:\n.byte ";
      for (unsigned I = 0; I < Bytes.size(); ++I)
        Native << (I ? "," : "") << unsigned(Bytes[I]);
      Native << "\n.globl _start\n_start:\ncall test_main\n";
      if (A == Arch::X86)
        Native << "mov %eax,%ebx\nmov $1,%eax\nint $0x80\n";
      else
        Native << "mov %eax,%edi\nmov $60,%eax\nsyscall\n";
      Native << ".section .note.GNU-stack,\"\",@progbits\n";
      Native.close();
      const auto HarnessPath = tmpFile("examine-harness.c");
      std::ofstream(HarnessPath) << R"(
#include <stdint.h>
#include <stddef.h>
void *memcpy(void *out, const void *in, size_t size) {
  unsigned char *dst = out;
  const unsigned char *src = in;
  for (size_t i = 0; i < size; ++i) dst[i] = src[i];
  return out;
}
void *memset(void *out, int value, size_t size) {
  unsigned char *dst = out;
  for (size_t i = 0; i < size; ++i) dst[i] = value;
  return out;
}
struct __attribute__((packed)) Bits { uint64_t sig; uint16_t exponent; };
extern unsigned native_examine(const struct Bits *);
extern unsigned examine_value(const struct Bits *);
int test_main(void) {
  const uint64_t significands[] = {0, 1, UINT64_C(0x7fffffffffffffff),
    UINT64_C(0x8000000000000000), UINT64_C(0x8000000000000001),
    UINT64_C(0xc000000000000000), UINT64_MAX};
  const unsigned exponents[] = {0, 1, 0x3fff, 0x7ffe, 0x7fff};
  for (unsigned i = 0; i != 7; ++i)
    for (unsigned j = 0; j != 5; ++j)
      for (unsigned sign = 0; sign != 2; ++sign) {
        const struct Bits value = {significands[i], exponents[j] | (sign << 15)};
        const unsigned expected = native_examine(&value);
        if (examine_value(&value) != expected) return 1 + i * 10 + j * 2 + sign;
      }
  return 0;
}
)";
      for (bool NoOpt : {false, true}) {
        SCOPED_TRACE(NoOpt);
        llvm::LLVMContext Context;
        PipelineOptions Options;
        Options.EmitDumpOutput = false;
        Options.LiftMode = true;
        Options.NoOpt = NoOpt;
        auto Lift = Pipeline().run(Image, Context, Options);
        ASSERT_TRUE(Lift.Success) << Lift.Error;
        ASSERT_NE(Lift.LlvmModule, nullptr);
        ASSERT_FALSE(llvm::verifyModule(*Lift.LlvmModule, &llvm::errs()));
        const auto IRPath = tmpFile("examine.ll");
        std::string IR;
        llvm::raw_string_ostream OS(IR);
        neverd::test::printHostCompilerFixture(*Lift.LlvmModule, OS);
        std::ofstream(IRPath) << IR;
        Options.LiftMode = false;
        auto High = Pipeline().run(Image, Context, Options);
        ASSERT_TRUE(High.Success) << High.Error;
        const auto CPath = tmpFile("examine.c");
        CEmitterOptions Emit;
        Emit.TheArch = A;
        Emit.Format = BinaryFormat::ELF;
        Emit.Image = &Image;
        std::string C;
        llvm::raw_string_ostream COS(C);
        ASSERT_TRUE(HighCEmitter().emit(High.HighFuncs, COS, Emit));
        std::ofstream(CPath) << C;
        for (const auto &Source : {IRPath, CPath})
          for (const std::string Opt : {"-O0", "-O2"}) {
            SCOPED_TRACE(Source.string());
            SCOPED_TRACE(Opt);
            const auto Binary = tmpFile("examine-exec");
            const auto Built =
                exec(NEVERD_TEST_CLANG,
                     {Target, Opt, "-ffreestanding", "-fno-strict-aliasing",
                      "-fuse-ld=lld", "-nostdlib", "-static", "-no-pie",
                      Source.string(), HarnessPath.string(),
                      NativePath.string(), "-o", Binary.string()});
            ASSERT_TRUE(Built.ok()) << Built.err << IR << C;
            const auto Ran = exec(Binary.string(), {});
            EXPECT_EQ(Ran.exitCode, 0) << Ran.err << IR << C;
          }
      }
    }
#endif
}

TEST(ImportCalleeTrace, RequiresTheUnchangedPointerAndInstructionTemporary) {
  for (Arch Architecture : {Arch::X86, Arch::X64, Arch::AArch64})
    for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
      SCOPED_TRACE(static_cast<int>(Architecture));
      SCOPED_TRACE(Mutation);
      const auto &TRI = getTargetRegInfo(Architecture);
      const NdVar Pointer = NdVar::reg(TRI.IntReturnReg, TRI.PointerSize);
      LowFunc Function;
      Function.Blocks.resize(1);
      LowBlock &Block = Function.Blocks[0];
      LowOp Load;
      Load.Addr = 0x1000;
      Load.Opcode = NdOp::LOAD;
      Load.Output = Mutation >= 5 ? NdVar::tmp(0, TRI.PointerSize) : Pointer;
      Load.addInput(NdVar::cst(0x2000, TRI.PointerSize));
      Block.Ops.push_back(Load);
      if (Mutation != 0) {
        LowOp Change;
        Change.Addr = Mutation == 5 ? 0x1000 : 0x1004;
        Change.Opcode = NdOp::COPY;
        Change.Output = Pointer;
        Change.addInput(NdVar::cst(0, TRI.PointerSize));
        if (Mutation == 1)
          Change.Output.Size = 1;
        if (Mutation == 2) {
          ++Change.Output.Offset;
          Change.Output.Size = 1;
        }
        if (Mutation == 3 || Mutation == 4) {
          Change.Opcode = Mutation == 3 ? NdOp::CALL : NdOp::INTRINSIC;
          Change.Output = {};
        }
        if (Mutation >= 5)
          Change.Inputs[0] = Load.Output;
        Block.Ops.push_back(Change);
      }
      LowOp Call;
      Call.Addr = 0x1008;
      Call.Opcode = NdOp::INDIR_CALL;
      Call.addInput(Pointer);
      Block.Ops.push_back(Call);
      EXPECT_EQ(loadedCallSlot(Function, Block, Block.Ops.size() - 1, Pointer),
                Mutation == 0 || Mutation == 5 ? std::optional<va_t>(0x2000)
                                               : std::nullopt);
    }
}

TEST(ImportCalleeTrace, WidenedLiteralAddressesKeepTheExactLoadSlot) {
  for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
    SCOPED_TRACE(Mutation);
    LowFunc Function;
    Function.Blocks.resize(1);
    auto &Block = Function.Blocks[0];
    const auto Address = NdVar::tmp(0, 4);
    const auto WideAddress = NdVar::tmp(8, 8);
    const auto Pointer = NdVar::reg(x86reg::RAX, 4);
    const uint64_t Literal = Mutation == 1 ? UINT64_C(0xffff2000) : 0x2000;
    auto Add = [&](NdOp Opcode, NdVar Output,
                   std::initializer_list<NdVar> Inputs) {
      LowOp Op;
      Op.Opcode = Opcode;
      Op.Addr = 0x1000;
      Op.Output = Output;
      for (auto Input : Inputs)
        Op.addInput(Input);
      Block.Ops.push_back(Op);
    };
    Add(NdOp::COPY, Address, {NdVar::cst(Literal, 4)});
    if (Mutation == 4)
      Add(NdOp::INT_ADD, Address, {Address, NdVar::cst(4, 4)});
    Add(Mutation == 2 ? NdOp::INT_SEXT : NdOp::INT_ZEXT, WideAddress,
        {Address});
    Add(NdOp::LOAD, Pointer, {WideAddress});
    NdVar Called = Pointer;
    if (Mutation == 3) {
      Called.Size = 8;
      Add(NdOp::INT_ZEXT, Called, {Pointer});
    }
    if (Mutation == 5)
      Block.Ops[1].Inputs[0].Size = 2; // partial temporary is not the address
    Add(NdOp::INDIR_CALL, {}, {Called});
    EXPECT_EQ(loadedCallSlot(Function, Block, Block.Ops.size() - 1, Called),
              Mutation < 2 ? std::optional<va_t>(Literal) : std::nullopt);
  }
}

TEST(X87CallStack, ProvesCrossBlockResultsBeforeExpandingLoopStates) {
  for (Arch Architecture : {Arch::X86, Arch::X64})
    for (BinaryFormat Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (unsigned Mutation = 0; Mutation != 16; ++Mutation) {
        SCOPED_TRACE(static_cast<int>(Architecture));
        SCOPED_TRACE(static_cast<int>(Format));
        SCOPED_TRACE(Mutation);
        BinaryImage Image = x87CallLoopImage(Architecture, Format, Mutation);
        Decoder Dec;
        ASSERT_TRUE(Dec.init(Image));
        CFGBuilder Builder;
        const std::set<va_t> Entries{0x1000, 0x1100, 0x1200};
        Builder.setKnownFuncEntries(&Entries);
        LowFunc Low = Builder.build(Image, Dec, 0x1000, "x87_call_loop");
        ASSERT_TRUE(Low.hasCompleteInstructionLift());
        if (Mutation == 14 || Mutation == 15) {
          const LowFunc Leaf = Builder.build(Image, Dec, 0x1200, "x87_leaf");
          ASSERT_TRUE(Leaf.hasCompleteInstructionLift());
          unsigned Guards = 0;
          for (const LowBlock &Block : Leaf.Blocks)
            for (const LowOp &Op : Block.Ops)
              if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
                  Op.Inputs[0].Offset ==
                      unsigned(Intrinsic::X86RequireDivPrecondition)) {
                ++Guards;
                EXPECT_TRUE(intrinsicX86DivPreconditionShapeIsValid(
                    Intrinsic::X86RequireDivPrecondition,
                    x86DivPreconditionLowShape(Op, Architecture)));
              }
          EXPECT_EQ(Guards, 1u);
        }
        if (ownsX87(Image)) {
          expectOwnedCallTransport(Low);
          EXPECT_FALSE(hasX87CallDefinition(Low));
          if (Mutation == 0 || Mutation >= 13) {
            const auto Wrapper =
                Builder.build(Image, Dec, 0x1100, "x87_forwarder");
            expectOwnedCallTransport(Wrapper);
            if (Mutation == 0) {
              Image.Segments[0].Data[0x201] = 0xee;
              expectOwnedCallTransport(
                  Builder.build(Image, Dec, 0x1100, "changed_constant"));
              Image.Segments[0].Data[0x200] = 0x90;
              Image.Segments[0].Data[0x201] = 0x90;
              const auto Empty =
                  Builder.build(Image, Dec, 0x1100, "changed_body");
              EXPECT_FALSE(hasX87CallDefinition(Empty));
              for (const auto &B : Empty.Blocks)
                for (const auto &Op : B.Ops)
                  EXPECT_FALSE(Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
                               Op.Inputs[0].Offset ==
                                   unsigned(Intrinsic::X87ContextBegin));
            }
          }
          continue;
        }
        const bool Proven = Mutation == 0 || Mutation >= 13;
        EXPECT_EQ(hasX87CallDefinition(Low), Proven);
        if (!Proven)
          continue;
        std::set<va_t> Starts;
        for (const LowBlock &B : Low.Blocks)
          EXPECT_TRUE(Starts.insert(B.StartAddr).second)
              << "a balanced call/consume loop needs no TOP-state copies";
        LowFunc Wrapper = Builder.build(Image, Dec, 0x1100, "x87_forwarder");
        EXPECT_TRUE(hasX87CallDefinition(Wrapper));
        // Reusing the builder after an in-place image edit must discard proofs.
        Image.Segments[0].Data[0x201] = 0xee; // fldz, still a defined result
        Low = Builder.build(Image, Dec, 0x1000, "x87_call_loop");
        EXPECT_TRUE(hasX87CallDefinition(Low));
        Image.Segments[0].Data[0x200] = 0x90;
        Image.Segments[0].Data[0x201] = 0x90; // nop; nop; ret
        Low = Builder.build(Image, Dec, 0x1000, "x87_call_loop");
        EXPECT_FALSE(hasX87CallDefinition(Low));
      }
}

TEST(X87CallStack, IncompleteCallClosureCannotAcquireAnEffect) {
  for (unsigned Count :
       {limits::kMaxX87CallProofDepth, limits::kMaxX87CallProofDepth + 1}) {
    SCOPED_TRACE(Count);
    BinaryImage Image = x87CallLoopImage(Arch::X86, BinaryFormat::ELF);
    Image.Symbols.resize(1);
    Image.KnownCodeRanges.resize(1);
    Segment &Text = Image.Segments[0];
    Text.Size = (Count + 1) * 0x100;
    Text.Data.resize(Text.Size, 0xcc);
    std::set<va_t> Entries{0x1000};
    for (unsigned I = 0; I < Count; ++I) {
      const va_t Address = 0x1100 + I * 0x100;
      const std::vector<uint8_t> Body =
          I + 1 == Count ? std::vector<uint8_t>{0xd9, 0xe8, 0xc3}
                         : std::vector<uint8_t>{0xe8, 0xfb, 0, 0, 0, 0xc3};
      std::copy(Body.begin(), Body.end(),
                Text.Data.begin() + Address - Text.VA);
      Image.Symbols.push_back(Symbol::makeFunc(Address, Body.size()));
      Image.KnownCodeRanges.emplace_back(Address, Address + Body.size());
      Entries.insert(Address);
    }
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Image));
    CFGBuilder Builder;
    Builder.setKnownFuncEntries(&Entries);
    const LowFunc Low = Builder.build(Image, Dec, 0x1000);
    ASSERT_TRUE(Low.hasCompleteInstructionLift());
    EXPECT_EQ(hasX87CallDefinition(Low),
              Count == limits::kMaxX87CallProofDepth);
  }
}

TEST(X87CallStack, AConsumingHelperCarriesItsIncomingPayloadAcrossALoop) {
  for (BinaryFormat Format :
       {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
    SCOPED_TRACE(static_cast<int>(Format));
    auto Image = x87ConsumingCallImage(Format);
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Image));
    CFGBuilder Builder;
    const std::set<va_t> Entries{0x1000, 0x1100};
    Builder.setKnownFuncEntries(&Entries);
    const auto Low = Builder.build(Image, Dec, 0x1000);
    EXPECT_TRUE(Low.hasCompleteInstructionLift());
    std::set<va_t> Starts;
    bool Found = false;
    for (const auto &Block : Low.Blocks) {
      EXPECT_TRUE(Starts.insert(Block.StartAddr).second);
      for (const auto &Op : Block.Ops)
        if (Op.Opcode == NdOp::CALL) {
          Found = true;
          ASSERT_EQ(Op.NumInputs, 2u);
          EXPECT_EQ(Op.Inputs[1].Size, 10u);
          EXPECT_TRUE(Op.Inputs[1].isReg());
          EXPECT_TRUE(
              getTargetRegInfo(Arch::X86).isX87StackReg(Op.Inputs[1].Offset));
          EXPECT_EQ(Op.Output.Offset, x86reg::RAX);
        }
    }
    EXPECT_TRUE(Found);
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.NoOpt = true;
    Options.LiftMode = true;
    Options.OnlyFunctionEntries = Entries;
    auto Result = Pipeline().run(Image, Context, Options);
    ASSERT_TRUE(Result.Success) << Result.Error;
    ASSERT_TRUE(Result.LlvmModule);
    EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
    const auto Helper = llvm::find_if(
        Result.MedFuncs, [](const MedFunc &F) { return F.Entry == 0x1100; });
    ASSERT_NE(Helper, Result.MedFuncs.end());
    ASSERT_EQ(Helper->Params.size(), 1u);
    EXPECT_EQ(Helper->Params[0].RegOff, x86reg::ST0);
    EXPECT_EQ(Helper->Params[0].Size, 10u);
    ASSERT_EQ(Helper->TypedParams.size(), 1u);
    EXPECT_EQ(Helper->TypedParams[0].Type->Kind, NdTypeKind::Float);
    const auto Caller = llvm::find_if(
        Result.MedFuncs, [](const MedFunc &F) { return F.Entry == 0x1000; });
    ASSERT_NE(Caller, Result.MedFuncs.end());
    ASSERT_EQ(Caller->CallInfos.size(), 1u);
    ASSERT_EQ(Caller->CallInfos[0].Args.size(), 1u);
    EXPECT_EQ(Caller->CallInfos[0].Args[0].Size, 10u);
    EXPECT_FALSE(Caller->FPReturnViaX87);
    Options.LiftMode = false;
    Options.PatchMode = true;
    auto Patch = Pipeline().run(Image, Context, Options);
    EXPECT_FALSE(Patch.Success);
    EXPECT_FALSE(Patch.LlvmModule);
    EXPECT_NE(Patch.Error.find("native x87 argument adapter"),
              std::string::npos);
    Options.LiftMode = true;
    Options.PatchMode = false;
    Image.CodeRefTargets.insert(0x1100);
    auto AddressTaken = Pipeline().run(Image, Context, Options);
    EXPECT_FALSE(AddressTaken.Success);
    EXPECT_FALSE(AddressTaken.LlvmModule);
    EXPECT_NE(AddressTaken.Error.find("x87 argument adapter"),
              std::string::npos);
  }
}

TEST(X87CallStack, ConsumersRequireCompleteConsistentStackAndPayloadProofs) {
  const std::vector<uint8_t> Pop{0x83, 0xec, 4, 0xdb, 0x1c, 0x24, 0x58, 0xc3};
  const std::vector<std::vector<uint8_t>> Prefixes{
      {},                             // consumes only ST0
      {0xd9, 0xc0, 0xde, 0xc1},       // one temporary, faddp consumes it again
      {0xdd, 0xd1},                   // writes caller's ST1
      {0xd8, 0xc1},                   // reads caller's ST1
      {0xdb, 0xe3},                   // resets the environment
      {0xe8, 0xfb, 0xff, 0xff, 0xff}, // recursive call
      {0xe8, 0xfb, 0, 0, 0},          // incomplete callee at 0x1200
      {0x85, 0xc0, 0x75, 1, 0xc3},    // one return leaves ST0 unconsumed
      {0xdd, 0xd8}, // pops the incoming value before reading it
      {0xe8, 0xfb, 0, 0,
       0},                // complete ordinary call has no ST preservation ABI
      {0xd9, 0x2c, 0x24}, // fldcw changes the caller's control state
  };
  for (unsigned I = 0; I < Prefixes.size(); ++I) {
    SCOPED_TRACE(I);
    auto Body = Prefixes[I];
    Body.insert(Body.end(), Pop.begin(), Pop.end());
    auto Image = x87ConsumingCallImage(BinaryFormat::ELF, Body);
    if (I == 9) {
      Image.Segments[0].Data[0x200] = 0xc3;
      Image.Symbols.push_back(Symbol::makeFunc(0x1200, 1));
      Image.KnownCodeRanges.emplace_back(0x1200, 0x1201);
    }
    auto Cache = createX87CallGraphCache();
    const std::set<va_t> Entries{0x1000, 0x1100, 0x1200};
    for (unsigned Repeat = 0; Repeat < 2; ++Repeat) {
      Decoder Dec;
      ASSERT_TRUE(Dec.init(Image));
      CFGBuilder Builder;
      Builder.setKnownFuncEntries(&Entries);
      Builder.setX87CallGraphCache(Cache);
      const auto Helper = Builder.build(Image, Dec, 0x1100);
      EXPECT_EQ(Helper.ConsumesX87Argument, I < 2);
      const auto Caller = Builder.build(Image, Dec, 0x1000);
      for (const auto &Block : Caller.Blocks)
        for (const auto &Op : Block.Ops)
          if (Op.Opcode == NdOp::CALL) {
            EXPECT_EQ(Op.ConsumesX87Argument, I < 2);
            if (I < 2)
              EXPECT_EQ(Op.X87ScratchSlots, I);
          }
    }
  }
}

TEST(X87CallStack, ConsumerCallsNeedAnArgumentAndEmptyScratchSlots) {
  const std::vector<uint8_t> Helper{0xd9, 0xc0, 0xde, 0xc1, // one scratch slot
                                    0x83, 0xec, 4,    0xdb,
                                    0x1c, 0x24, 0x58, 0xc3};
  for (bool ExhaustScratch : {false, true}) {
    auto Image = x87ConsumingCallImage(BinaryFormat::ELF, Helper);
    auto &Bytes = Image.Segments[0].Data;
    if (ExhaustScratch) {
      // Keep six more entries below the argument: all eight slots are full.
      Bytes.insert(Bytes.begin(), {0xd9, 0xe8, 0xd9, 0xe8, 0xd9, 0xe8, 0xd9,
                                   0xe8, 0xd9, 0xe8, 0xd9, 0xe8});
      Bytes.resize(Image.Segments[0].Size);
      // Keep the helper at its original entry, and adjust the direct call.
      std::fill(Bytes.begin() + 0x100, Bytes.begin() + 0x140, 0xcc);
      std::copy(Helper.begin(), Helper.end(), Bytes.begin() + 0x100);
      Image.Symbols[0].Size += 12;
      Image.KnownCodeRanges[0].second += 12;
      // FLD1 also contains E8, so find the call after the complete prefix.
      const auto Actual =
          std::find(Bytes.begin() + 42, Bytes.begin() + 0x100, 0xe8);
      ASSERT_NE(Actual, Bytes.begin() + 0x100);
      const uint32_t Disp = 0x1100 - (0x1000 + (Actual - Bytes.begin()) + 5);
      for (unsigned K = 0; K < 4; ++K)
        Actual[K + 1] = uint8_t(Disp >> (8 * K));
    } else {
      const std::array<uint8_t, 4> Load{0xdb, 0x44, 0x24, 12};
      const auto It =
          std::search(Bytes.begin(), Bytes.end(), Load.begin(), Load.end());
      ASSERT_NE(It, Bytes.end());
      std::fill(It, It + Load.size(), 0x90);
    }
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Image));
    CFGBuilder Builder;
    const std::set<va_t> Entries{0x1000, 0x1100};
    Builder.setKnownFuncEntries(&Entries);
    const auto Low = Builder.build(Image, Dec, 0x1000);
    EXPECT_FALSE(Low.hasCompleteInstructionLift());
    bool RejectedCall = false;
    for (const auto &Block : Low.Blocks)
      for (const auto &Op : Block.Ops)
        if (Op.ConsumesX87Argument)
          RejectedCall |=
              llvm::is_contained(Low.UnsupportedInstructionAddresses, Op.Addr);
    EXPECT_TRUE(RejectedCall);
  }
}

TEST(X87CallStack, ConsumerContractsCrossDirectAndTailWrappers) {
  for (bool Tail : {false, true}) {
    auto Image = x87ConsumingCallImage(BinaryFormat::ELF);
    wrapX87Consumer(Image, Tail);
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Image));
    CFGBuilder Builder;
    const std::set<va_t> Entries{0x1000, 0x1100, 0x1200};
    Builder.setKnownFuncEntries(&Entries);
    for (va_t Entry : {0x1000, 0x1100, 0x1200}) {
      const auto Low = Builder.build(Image, Dec, Entry);
      EXPECT_TRUE(Low.hasCompleteInstructionLift());
      EXPECT_EQ(Low.ConsumesX87Argument, Entry != 0x1000);
    }
  }
}

TEST(X87CallStack, AConsumerDoesNotPromiseTheRetainedEmptySlotPayload) {
  auto Image = x87ConsumingCallImage(
      BinaryFormat::ELF, {0xb8, 23, 0, 0, 0, 0xd9, 0xe1, 0xdd, 0xd8, 0xc3});
  // The helper takes fabs before popping, so an FXAM of its emptied slot
  // cannot reuse the caller's old negative payload sign.
  const std::vector<uint8_t> Caller{
      0x6a, 0xff, 0xdb, 0x04, 0x24, // push -1; fild [esp]
      0xe8, 0xf6, 0,    0,    0,    // call helper
      0xd9, 0xf7, 0xd9, 0xf7, 0xd9, 0xf7, 0xd9, 0xf7,
      0xd9, 0xf7, 0xd9, 0xf7, 0xd9, 0xf7, // seven fincstp reach the old ST0
      0xd9, 0xe5,                         // fxam
      0xdf, 0xe0, 0x59, 0xc3};            // fnstsw ax; pop ecx; ret
  auto &Bytes = Image.Segments[0].Data;
  std::fill(Bytes.begin(), Bytes.begin() + 0x100, 0xcc);
  std::copy(Caller.begin(), Caller.end(), Bytes.begin());
  Image.Symbols[0].Size = Caller.size();
  Image.KnownCodeRanges[0].second = 0x1000 + Caller.size();
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Image));
  CFGBuilder Builder;
  const std::set<va_t> Entries{0x1000, 0x1100};
  Builder.setKnownFuncEntries(&Entries);
  const auto Low = Builder.build(Image, Dec, 0x1000);
  EXPECT_TRUE(
      llvm::is_contained(Low.UnsupportedInstructionAddresses, va_t(0x1018)));
}

TEST(X87CallStack, MissingOrdinaryArgumentsCannotDisplaceTheX87Argument) {
  auto Image =
      x87ConsumingCallImage(BinaryFormat::ELF,
                            {0x83, 0xec, 4, 0xdb, 0x1c, 0x24, 0x58, 0x01, 0xc8,
                             0x01, 0xd0, 0x03, 0x44, 0x24, 4, 0xc3},
                            false, true);
  auto &Bytes = Image.Segments[0].Data;
  const std::array<uint8_t, 4> OutgoingStore{0x89, 0x04, 0x24, 0xb9};
  const auto It = std::search(Bytes.begin(), Bytes.end(), OutgoingStore.begin(),
                              OutgoingStore.end());
  ASSERT_NE(It, Bytes.end());
  // This loop-carried slot has no recovered argument value. Its absence
  // must remain explicit; the binary80 value still occupies the final slot.
  std::fill(It, It + 3, 0x90);
  for (bool NoOpt : {false, true}) {
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.NoOpt = NoOpt;
    Options.LiftMode = true;
    Options.OnlyFunctionEntries = {0x1000, 0x1100};
    auto Lift = Pipeline().run(Image, Context, Options);
    EXPECT_FALSE(Lift.Success);
    EXPECT_NE(Lift.Error.find("missing an ordinary argument"),
              std::string::npos)
        << Lift.Error;
    Options.LiftMode = false;
    auto Source = Pipeline().run(Image, Context, Options);
    ASSERT_TRUE(Source.Success) << Source.Error;
    const auto Caller = llvm::find_if(
        Source.MedFuncs, [](const MedFunc &F) { return F.Entry == 0x1000; });
    ASSERT_NE(Caller, Source.MedFuncs.end());
    ASSERT_EQ(Caller->CallInfos.size(), 1u);
    const auto &Args = Caller->CallInfos[0].Args;
    ASSERT_EQ(Args.size(), 4u);
    EXPECT_EQ(Args[2].Kind, MedVar::Unspecified);
    EXPECT_EQ(Args[3].Size, 10u);
    EXPECT_NE(Args[3].Kind, MedVar::Unspecified);
  }
}

TEST(X87CallStack, SelectedCallerKeepsTheConsumerDeclarationBinary80) {
  for (BinaryFormat Format :
       {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
    for (bool NoOpt : {false, true}) {
      auto Image = x87ConsumingCallImage(Format);
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.EmitDumpOutput = false;
      Options.NoOpt = NoOpt;
      Options.LiftMode = true;
      Options.OnlyFunctionEntries = {0x1000};
      auto Result = Pipeline().run(Image, Context, Options);
      ASSERT_TRUE(Result.Success) << Result.Error;
      ASSERT_TRUE(Result.LlvmModule);
      const auto *Callee = Result.LlvmModule->getFunction("sub_1100");
      ASSERT_TRUE(Callee);
      ASSERT_TRUE(Callee->isDeclaration());
      ASSERT_EQ(Callee->arg_size(), 1u);
      EXPECT_TRUE(Callee->getReturnType()->isIntegerTy(32));
      EXPECT_TRUE(Callee->getArg(0)->getType()->isX86_FP80Ty());
    }
}

TEST(X87CallGraphCache, IndependentBuildersRetainTheSameCallAndReturnIR) {
  for (auto [Architecture, Format] :
       {std::pair{Arch::X86, BinaryFormat::ELF},
        std::pair{Arch::X64, BinaryFormat::ELF},
        std::pair{Arch::X64, BinaryFormat::COFF}}) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    SCOPED_TRACE(static_cast<int>(Format));
    BinaryImage Image = x87CallLoopImage(Architecture, Format);
    const std::set<va_t> Entries{0x1000, 0x1100, 0x1200};
    auto Cache = createX87CallGraphCache();
    std::array<CFGBuilder, 3> Builders;
    std::array<std::string, 3> Dumps;
    size_t ColdHits = 0;
    for (size_t I = 0; I != Builders.size(); ++I) {
      Decoder Dec;
      ASSERT_TRUE(Dec.init(Image));
      CFGBuilder &Builder = Builders[I];
      Builder.setKnownFuncEntries(&Entries);
      if (I)
        Builder.setX87CallGraphCache(Cache);
      const LowFunc Low = Builder.build(Image, Dec, 0x1000, "x87_call_loop");
      ASSERT_TRUE(Low.hasCompleteInstructionLift());
      if (ownsX87(Image)) {
        expectOwnedCallTransport(Low);
        EXPECT_FALSE(hasX87CallDefinition(Low));
        EXPECT_EQ(detail::X87CallGraphCacheTestAccess::query(Builder, Image,
                                                             Image.Entry)
                      .first,
                  std::nullopt);
      } else
        EXPECT_TRUE(hasX87CallDefinition(Low));
      llvm::raw_string_ostream OS(Dumps[I]);
      Pipeline::dumpLowIR({Low}, OS);
      if (I == 1) {
        const auto Stats = detail::X87CallGraphCacheTestAccess::stats(Builder);
        EXPECT_GT(Stats[1], 0u);
        ColdHits = Stats[0];
      }
    }
    EXPECT_EQ(Dumps[0], Dumps[1]);
    EXPECT_EQ(Dumps[0], Dumps[2]);
    EXPECT_GT(detail::X87CallGraphCacheTestAccess::stats(Builders[2])[0],
              ColdHits);
  }
}

TEST(X87CallGraphCache, OwnedAndEqualInventoriesShareGraphsOthersDoNot) {
  using Access = detail::X87CallGraphCacheTestAccess;
  const BinaryImage Image = x87CallLoopImage(Arch::X64, BinaryFormat::ELF);
  const std::set<va_t> Entries{0x1000, 0x1100, 0x1200};
  const std::set<va_t> Copy = Entries;
  const std::set<va_t> Other{0x1000, 0x1100};
  auto Cache = createX87CallGraphCache(nullptr, nullptr, false,
                                       X87CallGraphCacheLimits(), &Entries);
  // Each build reports the cache's hits and graphs after it.
  const auto Build = [&](const std::set<va_t> &Known, bool Shared,
                         std::array<size_t, 3> *Stats = nullptr) {
    Decoder Dec;
    EXPECT_TRUE(Dec.init(Image));
    CFGBuilder Builder;
    Builder.setKnownFuncEntries(&Known);
    if (Shared)
      Builder.setX87CallGraphCache(Cache);
    std::string Dump;
    llvm::raw_string_ostream OS(Dump);
    Pipeline::dumpLowIR({Builder.build(Image, Dec, 0x1000, "x87_call_loop")},
                        OS);
    if (Stats)
      *Stats = Access::stats(Builder);
    return Dump;
  };
  const std::string Cold = Build(Entries, false);
  std::array<size_t, 3> First{}, Again{}, Equal{};
  EXPECT_EQ(Build(Entries, true, &First), Cold);
  ASSERT_GT(First[1], 0u);
  // The owner's inventory matches by identity; an equal set it does not own
  // matches by contents.  Both find the graphs the first build published.
  EXPECT_EQ(Build(Entries, true, &Again), Cold);
  EXPECT_GT(Again[0], First[0]);
  EXPECT_EQ(Build(Copy, true, &Equal), Cold);
  EXPECT_GT(Equal[0], Again[0]);
  // Another inventory binds other graphs, as a cold build would.
  EXPECT_EQ(Build(Other, true), Build(Other, false));
}

TEST(X87CallGraphCache, ContextsBorrowTheOwnedInventoryInsteadOfCopyingIt) {
  using Access = detail::X87CallGraphCacheTestAccess;
  const BinaryImage Image = x87CallLoopImage(Arch::X64, BinaryFormat::ELF);
  const std::set<va_t> Entries{0x1000, 0x1100, 0x1200};
  // One build in a cache that owns the inventory, one in a cache that does
  // not; each reports the cache's hits, graphs and retained bytes.
  const auto Build = [&](const std::set<va_t> *Owned,
                         std::array<size_t, 3> &Stats) {
    auto Cache = createX87CallGraphCache(nullptr, nullptr, false,
                                         X87CallGraphCacheLimits(), Owned);
    Decoder Dec;
    EXPECT_TRUE(Dec.init(Image));
    CFGBuilder Builder;
    Builder.setKnownFuncEntries(&Entries);
    Builder.setX87CallGraphCache(Cache);
    std::string Dump;
    llvm::raw_string_ostream OS(Dump);
    Pipeline::dumpLowIR({Builder.build(Image, Dec, 0x1000, "x87_call_loop")},
                        OS);
    Stats = Access::stats(Builder);
    return Dump;
  };
  std::array<size_t, 3> Borrowed{}, Copied{};
  EXPECT_EQ(Build(&Entries, Borrowed), Build(nullptr, Copied));
  ASSERT_GT(Borrowed[1], 0u);
  EXPECT_EQ(Borrowed[1], Copied[1]);
  // The same graphs; only the context that copies the inventory holds it.
  EXPECT_GE(Copied[2], Borrowed[2] + Entries.size() * sizeof(va_t));
}

TEST(X87CallGraphCache,
     PublishedCodeReferencesRefreshTagsWithoutChangingRawGraphs) {
  using Access = detail::X87CallGraphCacheTestAccess;
  for (Arch Architecture : {Arch::X86, Arch::X64})
    for (BinaryFormat Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
      SCOPED_TRACE(static_cast<int>(Architecture));
      SCOPED_TRACE(static_cast<int>(Format));
      // The successor initially inherits the full slot. Publishing its
      // address makes it an independent tag-proof entry with unknown state.
      auto Image =
          x87ExamineImage(Architecture, Format,
                          {0xd9, 0xe8, 0xeb, 0, 0xd9, 0xe5, 0xdf, 0xe0, 0xc3});
      const bool Owned = ownsX87(Image);
      const std::set<va_t> Entries{Image.Entry};
      const libc::NoReturnTargetIndex Names(Image);
      const InternalNoReturnIndex NoReturn(Image, &Entries, &Names, nullptr,
                                           nullptr);
      auto Cache = createX87CallGraphCache(&NoReturn, &Names);
      CFGBuilder Warm, Cold;
      for (auto *Builder : {&Warm, &Cold}) {
        Builder->setKnownFuncEntries(&Entries);
        Builder->setNoReturnTargetIndex(&Names);
        Builder->setNoReturnCalleeProver(&NoReturn);
      }
      Warm.setX87CallGraphCache(Cache);
      Decoder Dec;
      ASSERT_TRUE(Dec.init(Image));
      ASSERT_TRUE(
          Warm.build(Image, Dec, Image.Entry).hasCompleteInstructionLift());
      constexpr size_t Budget = limits::kMaxX87CallProofWork;
      const auto Original = Access::query(Warm, Image, Image.Entry, Budget);
      ASSERT_EQ(Original.first, Owned ? std::nullopt : std::optional<int>(-1));
      const size_t Work = Budget - Original.second;
      ASSERT_LT(Work, 1000u);
      const size_t Hits = Access::stats(Warm)[0];
      Image.CodeRefTargets.insert(0x1004);
      for (size_t Limit = 0; Limit <= Work; ++Limit)
        EXPECT_EQ(Access::query(Warm, Image, Image.Entry, Limit),
                  Access::query(Cold, Image, Image.Entry, Limit));
      EXPECT_GT(Access::stats(Warm)[0], Hits);

      const auto WarmLow = Warm.build(Image, Dec, Image.Entry);
      const auto ColdLow = Cold.build(Image, Dec, Image.Entry);
      EXPECT_EQ(WarmLow.hasCompleteInstructionLift(), Owned);
      EXPECT_EQ(WarmLow.UnsupportedInstructionAddresses,
                Owned ? std::vector<va_t>{} : std::vector<va_t>{0x1004});
      EXPECT_EQ(WarmLow.UnsupportedInstructionAddresses,
                ColdLow.UnsupportedInstructionAddresses);
      std::string WarmDump, ColdDump;
      llvm::raw_string_ostream WarmOS(WarmDump), ColdOS(ColdDump);
      Pipeline::dumpLowIR({WarmLow}, WarmOS);
      Pipeline::dumpLowIR({ColdLow}, ColdOS);
      EXPECT_EQ(WarmDump, ColdDump);
    }
}

TEST(X87CallGraphCache, WarmGraphsReplayEveryProofBudgetExactly) {
  using Access = detail::X87CallGraphCacheTestAccess;
  BinaryImage Image = x87CallLoopImage(Arch::X86, BinaryFormat::ELF);
  const std::set<va_t> Entries{0x1000, 0x1100, 0x1200};
  auto Cache = createX87CallGraphCache();
  CFGBuilder Seed;
  Seed.setKnownFuncEntries(&Entries);
  Seed.setX87CallGraphCache(Cache);
  constexpr size_t FullBudget = 4096;
  const auto Complete = Access::query(Seed, Image, 0x1100, FullBudget);
  ASSERT_EQ(Complete.first, std::optional<int>(-1));
  const size_t Cost = FullBudget - Complete.second;
  ASSERT_GT(Cost, 0u);
  ASSERT_LT(Cost, 512u);
  for (size_t Budget = 0; Budget <= Cost + 2; ++Budget) {
    SCOPED_TRACE(Budget);
    CFGBuilder Cold, Warm;
    Cold.setKnownFuncEntries(&Entries);
    Warm.setKnownFuncEntries(&Entries);
    Warm.setX87CallGraphCache(Cache);
    // Reaching the same answer is insufficient: skipping the cached graph's
    // work would change the point where an incomplete proof becomes complete.
    EXPECT_EQ(Access::query(Warm, Image, 0x1100, Budget),
              Access::query(Cold, Image, 0x1100, Budget));
  }
  EXPECT_GT(Access::stats(Seed)[0], 0u);
}

TEST(X87CallGraphCache, AnIncompleteProofDoesNotPoisonAnotherDepth) {
  using Access = detail::X87CallGraphCacheTestAccess;
  BinaryImage Image = x87CallLoopImage(Arch::X86, BinaryFormat::ELF);
  auto Cache = createX87CallGraphCache();
  CFGBuilder Shallow;
  Shallow.setX87CallGraphCache(Cache);
  const auto Incomplete =
      Access::query(Shallow, Image, 0x1100, limits::kMaxX87CallProofWork,
                    limits::kMaxX87CallProofDepth - 1);
  EXPECT_FALSE(Incomplete.first);
  ASSERT_GT(Access::stats(Shallow)[1], 0u);
  CFGBuilder Full, Cold;
  Full.setX87CallGraphCache(Cache);
  const auto Complete = Access::query(Full, Image, 0x1100);
  EXPECT_EQ(Complete.first, std::optional<int>(-1));
  EXPECT_EQ(Complete, Access::query(Cold, Image, 0x1100));
  EXPECT_GT(Access::stats(Full)[0], 0u);
  // Conversely, the earlier successful answer must not grant extra depth.
  CFGBuilder ShallowAgain;
  ShallowAgain.setX87CallGraphCache(Cache);
  EXPECT_EQ(Access::query(ShallowAgain, Image, 0x1100,
                          limits::kMaxX87CallProofWork,
                          limits::kMaxX87CallProofDepth - 1),
            Incomplete);
}

/// A function that calls 0x1100 and then holds bytes that are no
/// instruction: its lift is complete only when the callee is proved never to
/// return.
BinaryImage callThenNoInstructionImage() {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  Image.Entry = 0x1000;
  Segment Text;
  Text.Name = ".text";
  Text.VA = Image.Entry;
  Text.Size = 0x200;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0xcc);
  auto Publish = [&](va_t Address, const char *Name,
                     const std::vector<uint8_t> &Bytes) {
    std::copy(Bytes.begin(), Bytes.end(),
              Text.Data.begin() + Address - Text.VA);
    Symbol Sym = Symbol::makeFunc(Address, Bytes.size());
    Sym.Name = Name;
    Image.Symbols.push_back(Sym);
    Image.KnownCodeRanges.emplace_back(Address, Address + Bytes.size());
  };
  // call 0x1100; ff ff (no instruction); ret
  Publish(0x1000, "caller", {0xe8, 0xfb, 0, 0, 0, 0xff, 0xff, 0xc3});
  Publish(0x1100, "callee", {0xeb, 0xfe}); // jmp $
  Image.Segments.push_back(std::move(Text));
  return Image;
}

/// Answers whether a callee never returns as the test sets it.
struct SwitchedNoReturnProver final : NoReturnCalleeProver {
  bool NeverReturns = false;
  mutable size_t Questions = 0;
  bool neverReturns(va_t, unsigned) const override {
    ++Questions;
    return NeverReturns;
  }
  bool mayNeverReturn(va_t) const override { return true; }
};

TEST(X87CallGraphCache, AnIncompleteLiftIsBuiltOnceWhileItsAnswersHold) {
  // The caller's lift reaches the bytes after the call, which decode as no
  // instruction.  The refusal is kept with the answer that led there, so
  // another index answers from it instead of lifting the caller again.
  using Access = detail::X87CallGraphCacheTestAccess;
  const BinaryImage Image = callThenNoInstructionImage();
  const std::set<va_t> Entries{0x1000, 0x1100};
  SwitchedNoReturnProver Prover;
  auto Cache = createX87CallGraphCache();
  const auto Configure = [&](CFGBuilder &Builder, bool Shared) {
    Builder.setKnownFuncEntries(&Entries);
    Builder.setNoReturnCalleeProver(&Prover);
    if (Shared)
      Builder.setX87CallGraphCache(Cache);
  };
  CFGBuilder First, Second, Cold;
  Configure(First, true);
  Configure(Second, true);
  Configure(Cold, false);
  const auto Incomplete = Access::query(First, Image, 0x1000);
  EXPECT_EQ(Incomplete.first, std::nullopt);
  ASSERT_EQ(Access::stats(First)[1], 1u);
  const size_t Hits = Access::stats(First)[0];
  EXPECT_EQ(Access::query(Second, Image, 0x1000), Incomplete);
  EXPECT_EQ(Access::stats(Second)[0], Hits + 1);
  EXPECT_EQ(Access::stats(Second)[1], 1u);
  EXPECT_EQ(Access::query(Cold, Image, 0x1000), Incomplete);
}

TEST(X87CallGraphCache, AnIncompleteLiftIsBuiltAgainWhenAnAnswerChanges) {
  // Once the callee is proved never to return, the caller's lift stops at
  // the call.  The refusal no longer holds: the request lifts the caller
  // again, and its complete graph replaces the refusal.
  using Access = detail::X87CallGraphCacheTestAccess;
  const BinaryImage Image = callThenNoInstructionImage();
  const std::set<va_t> Entries{0x1000, 0x1100};
  SwitchedNoReturnProver Prover;
  auto Cache = createX87CallGraphCache();
  const auto Configure = [&](CFGBuilder &Builder, bool Shared) {
    Builder.setKnownFuncEntries(&Entries);
    Builder.setNoReturnCalleeProver(&Prover);
    if (Shared)
      Builder.setX87CallGraphCache(Cache);
  };
  CFGBuilder Seed;
  Configure(Seed, true);
  const auto Incomplete = Access::query(Seed, Image, 0x1000);
  ASSERT_EQ(Incomplete.first, std::nullopt);
  Prover.NeverReturns = true;
  CFGBuilder Warm, Cold, Again;
  Configure(Warm, true);
  Configure(Cold, false);
  Configure(Again, true);
  const auto Complete = Access::query(Warm, Image, 0x1000);
  EXPECT_NE(Complete, Incomplete);
  EXPECT_EQ(Complete, Access::query(Cold, Image, 0x1000));
  // The complete graph answers without another proof or lift.
  const size_t Hits = Access::stats(Warm)[0];
  const size_t Questions = Prover.Questions;
  EXPECT_EQ(Access::query(Again, Image, 0x1000), Complete);
  EXPECT_EQ(Prover.Questions, Questions);
  EXPECT_GT(Access::stats(Again)[0], Hits);
}

TEST(X87CallGraphCache, AReusedBodyProvesEachCallOnceWhileItsAnswersHold) {
  // 0x1180 calls the caller of callThenNoInstructionImage, whose lift is
  // complete only once 0x1100 is proved never to return.  Reusing the
  // retained body asks whether that call may change the x87 stack.  The
  // answer is kept with the no-return answer the incomplete lift read: a
  // later reuse reads no graph, and once 0x1100 never returns the call is
  // proved again.
  using Access = detail::X87CallGraphCacheTestAccess;
  BinaryImage Image = callThenNoInstructionImage();
  // call 0x1000; ret
  const std::vector<uint8_t> Outer = {0xe8, 0x7b, 0xfe, 0xff, 0xff, 0xc3};
  std::copy(Outer.begin(), Outer.end(), Image.Segments[0].Data.begin() + 0x180);
  Symbol Sym = Symbol::makeFunc(0x1180, Outer.size());
  Sym.Name = "outer";
  Image.Symbols.push_back(Sym);
  Image.KnownCodeRanges.emplace_back(0x1180, 0x1180 + Outer.size());
  const std::set<va_t> Entries{0x1000, 0x1100, 0x1180};
  struct TargetProver final : NoReturnCalleeProver {
    std::set<va_t> Never;
    mutable size_t Questions = 0;
    bool neverReturns(va_t Target, unsigned) const override {
      ++Questions;
      return Never.count(Target) != 0;
    }
    bool mayNeverReturn(va_t) const override { return true; }
  } Prover;
  auto Cache = createX87CallGraphCache(&Prover, nullptr,
                                       /*RetainCallRegisterBodies=*/true);
  CFGBuilder::warmCallGraphs(Cache, Image, {0x1180}, &Entries, nullptr, &Prover,
                             nullptr, nullptr, 1);
  CFGBuilder Probe;
  Probe.setX87CallGraphCache(Cache);
  const auto Reuse = [&] {
    return CFGBuilder::cachedCallRegisterBody(
        Cache, Image, 0x1180, &Entries, nullptr, &Prover, nullptr, nullptr);
  };
  ASSERT_TRUE(Reuse());
  const auto Graphs = Access::stats(Probe);
  const size_t Questions = Prover.Questions;
  ASSERT_TRUE(Reuse());
  EXPECT_EQ(Access::stats(Probe), Graphs);
  EXPECT_GT(Prover.Questions, Questions);
  Prover.Never = {0x1100};
  ASSERT_TRUE(Reuse());
  EXPECT_GT(Access::stats(Probe)[0], Graphs[0]);
}

TEST(X87CallGraphCache, ChangedBuildContextsCannotHitThePreviousGraph) {
  using Access = detail::X87CallGraphCacheTestAccess;
  struct ReturningProver final : NoReturnCalleeProver {
    bool neverReturns(va_t, unsigned) const override { return false; }
  } FirstProver, OtherProver;
  for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
    SCOPED_TRACE(Mutation);
    BinaryImage Image = x87CallLoopImage(Arch::X86, BinaryFormat::ELF);
    BinaryImage OtherImage = x87CallLoopImage(Arch::X86, BinaryFormat::ELF, 5);
    std::set<va_t> Entries{0x1000, 0x1100, 0x1200}, Protected, Unsafe;
    auto Cache = createX87CallGraphCache();
    unsigned Depth = 0;
    const NoReturnCalleeProver *Prover = &FirstProver;
    const auto Configure = [&](CFGBuilder &Builder) {
      Builder.setKnownFuncEntries(&Entries);
      Builder.setProtectedJumpTableRelocationSlots(&Protected);
      Builder.setUnsafeJumpTableBranches(&Unsafe);
      Builder.setNoReturnCalleeProver(Prover, Depth);
    };
    CFGBuilder Seed;
    Configure(Seed);
    Seed.setX87CallGraphCache(Cache);
    ASSERT_EQ(Access::query(Seed, Image, 0x1200).first, std::optional<int>(-1));
    const auto Before = Access::stats(Seed);
    const BinaryImage *CurrentImage = &Image;
    if (Mutation == 0)
      Depth = 1;
    else if (Mutation == 1)
      Entries.insert(0x1250); // Same pointer and new contents.
    else if (Mutation == 2)
      Protected.insert(0x1210);
    else if (Mutation == 3)
      Unsafe.insert(0x1220);
    else if (Mutation == 4)
      CurrentImage = &OtherImage; // Same VA, different floating stack effect.
    else if (Mutation == 5)
      Prover = &OtherProver;
    CFGBuilder Warm, Cold;
    Configure(Warm);
    Configure(Cold);
    for (CFGBuilder *Builder : {&Warm, &Cold}) {
      if (Mutation == 6)
        Builder->setKnownFuncEntries(nullptr);
      else if (Mutation == 7)
        Builder->setProtectedJumpTableRelocationSlots(nullptr);
      else if (Mutation == 8)
        Builder->setUnsafeJumpTableBranches(nullptr);
    }
    Warm.setX87CallGraphCache(Cache);
    const auto Actual = Access::query(Warm, *CurrentImage, 0x1200);
    EXPECT_EQ(Actual, Access::query(Cold, *CurrentImage, 0x1200));
    EXPECT_EQ(Actual.first, std::optional<int>(Mutation == 4 ? 0 : -1));
    EXPECT_EQ(Access::stats(Warm)[0], Before[0]);
  }
}

BinaryImage x87RejectedGraphImage(Arch Architecture, bool Exceptional) {
  BinaryImage Image;
  Image.Arch = Architecture;
  Image.Bits = Architecture == Arch::X86 ? Bitness::Bits32 : Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  Image.Entry = 0x1200;
  Segment Text;
  Text.Name = ".text";
  Text.VA = Image.Entry;
  Text.Size = 0x20;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0xcc);
  Text.Data[0] = Exceptional ? 0xd9 : 0xff;
  Text.Data[1] = Exceptional ? 0xe8 : 0xe0; // fld1 or jmp eax/rax
  Text.Data[2] = 0xc3;
  if (Exceptional) {
    Text.Data[0x10] = 0xd9;
    Text.Data[0x11] = 0xe8;
    Text.Data[0x12] = 0xc3;
    ExceptionFunction EH;
    EH.CodeRange = {Image.Entry, Image.Entry + Text.Size};
    EH.Encoding = ExceptionEncoding::DwarfFDE;
    EH.Personality = ExceptionPersonality::GxxPersonalityV0;
    EH.Itanium.emplace();
    ItaniumCallSite Site;
    Site.GuardedRange = {Image.Entry, Image.Entry + 2};
    Site.LandingPadVA = Image.Entry + 0x10;
    EH.Itanium->CallSites.push_back(Site);
    Image.ExceptionMetadata.Functions.push_back(std::move(EH));
    Image.ExceptionMetadata.rebuildIndex();
  }
  Image.Symbols.push_back(Symbol::makeFunc(Image.Entry, Text.Size));
  Image.KnownCodeRanges.emplace_back(Image.Entry, Image.Entry + Text.Size);
  Image.Segments.push_back(std::move(Text));
  return Image;
}

namespace {
BinaryImage registerBodyImage(BinaryFormat Format,
                              const std::vector<uint8_t> &Bytes,
                              const std::vector<uint8_t> &Callee = {0xc3}) {
  auto Image = x87ExamineImage(Arch::X64, Format, Bytes);
  auto &Text = Image.Segments.front();
  Text.Data.resize(0x100 + Callee.size(), 0xcc);
  std::copy(Callee.begin(), Callee.end(), Text.Data.begin() + 0x100);
  Text.Size = Text.FileSz = Text.Data.size();
  Image.Symbols.push_back(Symbol::makeFunc(0x1100, Callee.size()));
  Image.KnownCodeRanges.emplace_back(0x1100, 0x1100 + Callee.size());
  return Image;
}

std::shared_ptr<const CallRegisterBody>
cachedRegisterBody(const std::shared_ptr<X87CallGraphCache> &Cache,
                   const BinaryImage &Image,
                   const std::set<va_t> *Entries = nullptr,
                   const NoReturnCalleeProver *Prover = nullptr) {
  return CFGBuilder::cachedCallRegisterBody(Cache, Image, Image.Entry, Entries,
                                            nullptr, Prover, nullptr, nullptr);
}

std::optional<LowFunc>
cachedFunction(const std::shared_ptr<X87CallGraphCache> &Cache,
               const BinaryImage &Image,
               const std::set<va_t> *Entries = nullptr,
               const NoReturnCalleeProver *Prover = nullptr,
               const std::string &Name = "") {
  return CFGBuilder::cachedDefaultFunction(Cache, Image, Image.Entry, Name,
                                           Entries, nullptr, Prover, nullptr,
                                           nullptr);
}

void expectSameRegisterBody(const LowFunc &Cold, const CallRegisterBody &Warm) {
  EXPECT_EQ(Warm.Complete, Cold.hasCompleteLiftCoverage());
  EXPECT_EQ(Warm.UnsafeIndirectBranches,
            !Cold.UnsafeIndirectBranchAddresses.empty());
  ASSERT_EQ(Warm.Blocks.size(), Cold.Blocks.size());
  for (size_t I = 0; I < Cold.Blocks.size(); ++I) {
    const auto &A = Cold.Blocks[I], &B = Warm.Blocks[I];
    EXPECT_EQ(A.Id, B.Id);
    EXPECT_EQ(A.StartAddr, B.StartAddr);
    EXPECT_EQ(A.EndAddr, B.EndAddr);
    EXPECT_EQ(A.Preds, B.Preds);
    EXPECT_EQ(A.Succs, B.Succs);
    EXPECT_EQ(A.ExceptionalPreds, B.ExceptionalPreds);
    EXPECT_EQ(A.ExceptionalSuccs, B.ExceptionalSuccs);
    ASSERT_EQ(A.Ops.size(), B.Ops.size());
    for (size_t J = 0; J < A.Ops.size(); ++J) {
      const auto &X = A.Ops[J], &Y = B.Ops[J];
      EXPECT_EQ(X.Opcode, Y.Opcode);
      EXPECT_EQ(X.Output, Y.Output);
      ASSERT_EQ(X.NumInputs, Y.NumInputs);
      for (unsigned K = 0; K < X.NumInputs; ++K)
        EXPECT_EQ(X.Inputs[K], Y.Inputs[K]);
      EXPECT_EQ(X.Addr, Y.Addr);
      EXPECT_EQ(X.Seq, Y.Seq);
      EXPECT_EQ(X.ConsumesX87Argument, Y.ConsumesX87Argument);
      EXPECT_EQ(X.X87ScratchSlots, Y.X87ScratchSlots);
      EXPECT_EQ(X.X87ImportArguments, Y.X87ImportArguments);
      EXPECT_EQ(X.MemoryOrdering, Y.MemoryOrdering);
      EXPECT_EQ(X.MemoryAddressSpace, Y.MemoryAddressSpace);
    }
    ASSERT_EQ(A.InstructionBoundaries.size(), B.InstructionBoundaries.size());
    for (size_t J = 0; J < A.InstructionBoundaries.size(); ++J) {
      const auto &X = A.InstructionBoundaries[J];
      const auto &Y = B.InstructionBoundaries[J];
      EXPECT_EQ(X.Address, Y.Address);
      EXPECT_EQ(X.Size, Y.Size);
      EXPECT_EQ(X.FirstOp, Y.FirstOp);
      EXPECT_EQ(X.OpCount, Y.OpCount);
      EXPECT_EQ(X.Mode, Y.Mode);
      EXPECT_EQ(X.Control, Y.Control);
      EXPECT_EQ(X.ControlFlags, Y.ControlFlags);
      EXPECT_EQ(X.TargetMode, Y.TargetMode);
      EXPECT_EQ(X.Immediate, Y.Immediate);
    }
  }
}

void expectSameFunctionFacts(const LowFunc &Cold, const LowFunc &Warm) {
  CallRegisterBody Body;
  Body.Blocks = Warm.Blocks;
  Body.Complete = Warm.hasCompleteLiftCoverage();
  Body.UnsafeIndirectBranches = !Warm.UnsafeIndirectBranchAddresses.empty();
  expectSameRegisterBody(Cold, Body);
  EXPECT_EQ(Warm.Entry, Cold.Entry);
  EXPECT_EQ(Warm.OriginalSize, Cold.OriginalSize);
  EXPECT_EQ(Warm.Name, Cold.Name);
  EXPECT_EQ(Warm.DebugName, Cold.DebugName);
  EXPECT_EQ(Warm.SourceFile, Cold.SourceFile);
  EXPECT_EQ(Warm.SourceLine, Cold.SourceLine);
  EXPECT_EQ(Warm.ModuleAnalysisRoots, Cold.ModuleAnalysisRoots);
  EXPECT_EQ(Warm.OrdinaryModuleAnalysisRoots, Cold.OrdinaryModuleAnalysisRoots);
  EXPECT_EQ(Warm.EverPublishedJumpTableBranchAddresses,
            Cold.EverPublishedJumpTableBranchAddresses);
  EXPECT_EQ(Warm.UnsafeIndirectBranchAddresses,
            Cold.UnsafeIndirectBranchAddresses);
  EXPECT_EQ(Warm.RelocatedInstructionAddressOccurrences,
            Cold.RelocatedInstructionAddressOccurrences);
  EXPECT_EQ(Warm.RelocatedInstructionScalarModelOccurrences,
            Cold.RelocatedInstructionScalarModelOccurrences);
  EXPECT_EQ(Warm.I386GetPcOccurrences, Cold.I386GetPcOccurrences);
  EXPECT_EQ(Warm.CodeRefTargets, Cold.CodeRefTargets);
  EXPECT_EQ(Warm.DecodedInstructionCount, Cold.DecodedInstructionCount);
  EXPECT_EQ(Warm.LiftedInstructionCount, Cold.LiftedInstructionCount);
  EXPECT_EQ(Warm.DecodeFailureAddresses, Cold.DecodeFailureAddresses);
  EXPECT_EQ(Warm.UnsupportedInstructionAddresses,
            Cold.UnsupportedInstructionAddresses);
  EXPECT_EQ(Warm.UnprovenReturnAddresses, Cold.UnprovenReturnAddresses);
  EXPECT_EQ(Warm.TruncatedPathAddresses, Cold.TruncatedPathAddresses);
  EXPECT_EQ(Warm.CalleePopBytes, Cold.CalleePopBytes);
  ASSERT_EQ(Warm.FunctionTemporaries.size(), Cold.FunctionTemporaries.size());
  for (size_t I = 0; I < Cold.FunctionTemporaries.size(); ++I) {
    EXPECT_EQ(Warm.FunctionTemporaries[I].Offset,
              Cold.FunctionTemporaries[I].Offset);
    EXPECT_EQ(Warm.FunctionTemporaries[I].Bytes,
              Cold.FunctionTemporaries[I].Bytes);
  }
  ASSERT_EQ(Warm.JumpTables.size(), Cold.JumpTables.size());
  for (size_t I = 0; I < Cold.JumpTables.size(); ++I) {
    const auto &A = Cold.JumpTables[I], &B = Warm.JumpTables[I];
    EXPECT_EQ(B.InsnAddr, A.InsnAddr);
    EXPECT_EQ(B.BaseAddr, A.BaseAddr);
    EXPECT_EQ(B.HasBaseAddr, A.HasBaseAddr);
    EXPECT_EQ(B.EntrySize, A.EntrySize);
    EXPECT_EQ(B.EntryStride, A.EntryStride);
    EXPECT_EQ(B.StorageRanges, A.StorageRanges);
    EXPECT_EQ(B.ExactPhysicalStorageRange, A.ExactPhysicalStorageRange);
    EXPECT_EQ(B.SuppressibleRelocationSlots, A.SuppressibleRelocationSlots);
    EXPECT_EQ(B.AuthenticatedTableLoads, A.AuthenticatedTableLoads);
    EXPECT_EQ(B.SelectorUseRefs, A.SelectorUseRefs);
    EXPECT_EQ(B.CompositeSelectorUseRef, A.CompositeSelectorUseRef);
    EXPECT_EQ(B.IndexRegOff, A.IndexRegOff);
    EXPECT_EQ(B.IsRelative, A.IsRelative);
    EXPECT_EQ(B.IsSigned, A.IsSigned);
    EXPECT_EQ(B.TargetBase, A.TargetBase);
    EXPECT_EQ(B.HasTargetBase, A.HasTargetBase);
    EXPECT_EQ(B.IsPEImageRelativeRVA, A.IsPEImageRelativeRVA);
    EXPECT_EQ(B.TableLoadAddr, A.TableLoadAddr);
    EXPECT_EQ(B.PreScaledIndex, A.PreScaledIndex);
    EXPECT_EQ(B.TwoTableSelect, A.TwoTableSelect);
    EXPECT_EQ(B.TwoTableOffset, A.TwoTableOffset);
    EXPECT_EQ(B.TwoTableHiPositive, A.TwoTableHiPositive);
    EXPECT_EQ(B.TwoLevelIndex, A.TwoLevelIndex);
    EXPECT_EQ(B.MutatedUnsafe, A.MutatedUnsafe);
    EXPECT_EQ(B.Targets, A.Targets);
    EXPECT_EQ(B.SlotIndices, A.SlotIndices);
    EXPECT_EQ(B.HasDispatchSlotMap, A.HasDispatchSlotMap);
    EXPECT_EQ(B.CaseLabels, A.CaseLabels);
  }
  EXPECT_EQ(Warm.ExceptionMetadata.has_value(),
            Cold.ExceptionMetadata.has_value());
  EXPECT_EQ(Warm.RegistrationStates.has_value(),
            Cold.RegistrationStates.has_value());
  EXPECT_EQ(Warm.CxxContinuationExitAnalysisComplete,
            Cold.CxxContinuationExitAnalysisComplete);
  EXPECT_EQ(Warm.CxxContinuationExits.size(), Cold.CxxContinuationExits.size());
  EXPECT_EQ(Warm.CxxContinuationEntries.size(),
            Cold.CxxContinuationEntries.size());
  EXPECT_EQ(Warm.SEHLocalUnwindContinuations.size(),
            Cold.SEHLocalUnwindContinuations.size());
}
TEST_F(X86_32_X87FPU,
       NativeAndLiftedConsumersPreserveTheRemainingStackAndBinary80) {
#if !defined(__linux__) || !defined(__x86_64__)
  GTEST_SKIP() << "requires a Linux x86-64 host with native i386 execution";
#else
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "requires Clang's i386 target";
  const std::vector<std::vector<uint8_t>> Helpers{
      {0x83, 0xec, 4, 0xdb, 0x1c, 0x24, 0x58, 0xc3},
      {0xd9, 0xc0, 0xde, 0xc1, 0x83, 0xec, 4, 0xdb, 0x1c, 0x24, 0x58, 0xc3},
      {0x83, 0xec, 0x10, 0xd9, 0xe8, 0xde, 0xe9, // subtract one exactly
       0xdb, 0x3c, 0x24, 0x0f, 0xb7, 0x44, 0x24, 8, 0x83, 0xc4, 0x10, 0xc3},
      {0xb8, 23, 0, 0, 0, 0xd9, 0xe1, 0xdd, 0xd8, 0xc3},
      {0x83, 0xec, 4, 0xdb, 0x1c, 0x24, 0x58,               // fistp; pop eax
       0x01, 0xc8, 0x01, 0xd0, 0x03, 0x44, 0x24, 4, 0xc3}}; // ecx+edx+stack
  const std::array<unsigned, 8> Expected{13, 19, 0x3fc3, 13, 13, 76, 121, 121};
  const std::array<unsigned, 8> HelperForCase{0, 1, 2, 0, 0, 3, 4, 4};
  for (unsigned Case = 0; Case < Expected.size(); ++Case)
    for (bool NoOpt : {false, true}) {
      SCOPED_TRACE(Case);
      SCOPED_TRACE(NoOpt);
      auto Image =
          x87ConsumingCallImage(BinaryFormat::ELF, Helpers[HelperForCase[Case]],
                                Case == 2, Case >= 6);
      const bool Wrapper = Case == 3 || Case == 4 || Case == 7;
      if (Wrapper)
        wrapX87Consumer(Image, Case != 3);
      // Selected modules use the original entry's automatic name for an
      // omitted callee. Give the separately emitted definition that name.
      Image.Symbols[1].Name = "sub_1100";
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.EmitDumpOutput = false;
      Options.LiftMode = true;
      Options.NoOpt = NoOpt;
      Options.OnlyFunctionEntries = {0x1000, 0x1100};
      if (Wrapper)
        Options.OnlyFunctionEntries.insert(0x1200);
      auto Lift = Pipeline().run(Image, Context, Options);
      if (!Lift.Success)
        Pipeline::dumpMedIR(Lift.MedFuncs, llvm::errs());
      ASSERT_TRUE(Lift.Success) << Lift.Error;
      ASSERT_TRUE(Lift.LlvmModule);
      ASSERT_FALSE(llvm::verifyModule(*Lift.LlvmModule, &llvm::errs()));
      for (const MedFunc &Function : Lift.MedFuncs)
        if (Function.ConsumesX87Argument) {
          const auto *Emitted = Lift.LlvmModule->getFunction(Function.Name);
          ASSERT_TRUE(Emitted);
          ASSERT_FALSE(Emitted->arg_empty());
          EXPECT_TRUE(Emitted->getArg(Emitted->arg_size() - 1)
                          ->getType()
                          ->isX86_FP80Ty());
        }
      const auto LLVMFile = tmpFile("consumer.ll");
      std::string IR;
      llvm::raw_string_ostream OS(IR);
      neverd::test::printHostCompilerFixture(*Lift.LlvmModule, OS);
      std::ofstream(LLVMFile) << IR;
      const auto CallerFile = tmpFile("consumer-caller.ll");
      const auto CalleeFile = tmpFile("consumer-callee.ll");
      for (bool CallerOnly : {true, false}) {
        auto SplitOptions = Options;
        if (CallerOnly)
          SplitOptions.OnlyFunctionEntries = {0x1000};
        else
          SplitOptions.OnlyFunctionEntries.erase(0x1000);
        auto Part = Pipeline().run(Image, Context, SplitOptions);
        ASSERT_TRUE(Part.Success) << Part.Error;
        ASSERT_TRUE(Part.LlvmModule);
        ASSERT_FALSE(llvm::verifyModule(*Part.LlvmModule, &llvm::errs()));
        std::string PartIR;
        llvm::raw_string_ostream PartOS(PartIR);
        neverd::test::printHostCompilerFixture(*Part.LlvmModule, PartOS);
        std::ofstream(CallerOnly ? CallerFile : CalleeFile) << PartIR;
      }
      Options.LiftMode = false;
      auto High = Pipeline().run(Image, Context, Options);
      ASSERT_TRUE(High.Success) << High.Error;
      const auto CFile = tmpFile("consumer.c");
      std::string C = "#include <stdint.h>\n#include <stddef.h>\n";
      llvm::raw_string_ostream COS(C);
      CEmitterOptions Emit;
      Emit.TheArch = Arch::X86;
      Emit.Format = BinaryFormat::ELF;
      Emit.Image = &Image;
      Emit.EmitIncludes = false;
      ASSERT_TRUE(HighCEmitter().emit(High.HighFuncs, COS, Emit));
      std::ofstream(CFile) << C;
      const auto NativeFile = tmpFile("consumer.s");
      std::ofstream Native(NativeFile);
      Native << ".text\n.globl x87_call_loop\nx87_call_loop:\n.byte ";
      const auto &Bytes = Image.Segments[0].Data;
      for (size_t I = 0; I < Bytes.size(); ++I)
        Native << (I ? "," : "") << unsigned(Bytes[I]);
      Native << "\n.section .note.GNU-stack,\"\",@progbits\n";
      Native.close();
      const auto HarnessFile = tmpFile("consumer-start.s");
      std::ofstream(HarnessFile)
          << ".text\n.globl _start\n_start:\nfninit\ncall x87_call_loop\ncmpl $"
          << Expected[Case]
          << ",%eax\nsetne %bl\nmovzbl %bl,%ebx\nmovl $1,%eax\nint $0x80\n"
             ".section .note.GNU-stack,\"\",@progbits\n";
      for (const std::string Opt : {"-O0", "-O2"})
        for (const auto &Sources : std::vector<std::vector<std::string>>{
                 {NativeFile.string()},
                 {LLVMFile.string()},
                 {CFile.string()},
                 {CallerFile.string(), CalleeFile.string()}}) {
          SCOPED_TRACE(Opt);
          SCOPED_TRACE(Sources.front());
          const auto Binary = tmpFile("consumer-execution");
          std::vector<std::string> BuildArgs{"--target=i386-linux-gnu",
                                             Opt,
                                             "-ffreestanding",
                                             "-fno-strict-aliasing",
                                             "-fuse-ld=lld",
                                             "-nostdlib",
                                             "-static",
                                             "-no-pie"};
          BuildArgs.insert(BuildArgs.end(), Sources.begin(), Sources.end());
          BuildArgs.insert(BuildArgs.end(),
                           {HarnessFile.string(), "-o", Binary.string()});
          const auto Build = exec(NEVERD_TEST_CLANG, BuildArgs);
          ASSERT_TRUE(Build.ok()) << Build.err << IR << C;
          const auto Run = exec(Binary.string(), {});
          ASSERT_TRUE(Run.ok()) << Run.err << IR << C;
        }
    }
#endif
}
} // namespace

TEST(CompleteFunctionCache, DefaultBodiesPreserveFactsAndUseTheRequestedName) {
  using Access = detail::X87CallGraphCacheTestAccess;
  for (BinaryFormat Format :
       {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
    auto Image = registerBodyImage(
        Format, {0x48, 0x8d, 0x05, 0xf9, 0, 0, 0, 0x85, 0xff, 0x74, 1, 0xc3,
                 0x31, 0xc0, 0xc3}); // address-taken callee; two returns
    const std::set<va_t> Entries{0x1000, 0x1100};
    auto Cache = createX87CallGraphCache(nullptr, nullptr, true);
    CFGBuilder Seed, Cold;
    Seed.setKnownFuncEntries(&Entries);
    Cold.setKnownFuncEntries(&Entries);
    Seed.setX87CallGraphCache(Cache);
    Access::query(Seed, Image, Image.Entry);
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Image));
    for (const std::string Name :
         {std::string{}, std::string("selected_name")}) {
      auto Warm = cachedFunction(Cache, Image, &Entries, nullptr, Name);
      ASSERT_TRUE(Warm);
      const auto Original = Cold.build(Image, Dec, Image.Entry, Name);
      ASSERT_FALSE(Original.CodeRefTargets.empty());
      expectSameFunctionFacts(Original, *Warm);
      // A consumer may mutate its own output without changing the cache.
      Warm->Blocks.clear();
      EXPECT_FALSE(cachedFunction(Cache, Image, &Entries)->Blocks.empty());
    }
  }
}

TEST(CompleteFunctionCache, JumpTableStorageAndOccurrenceMetadataSurviveReuse) {
  using Access = detail::X87CallGraphCacheTestAccess;
  // Bound the full RDI used by the address, and authenticate the fixed linked
  // table address. A 32-bit CMP alone would leave its upper address bits free.
  auto Image = registerBodyImage(
      BinaryFormat::ELF,
      {0x48, 0x83, 0xff, 1,    0x77, 0x16, 0xff, 0x24, 0xfd, 0, 0x20,
       0,    0,    0xcc, 0xcc, 0xcc, 0xb8, 1,    0,    0,    0, 0xc3,
       0xb8, 2,    0,    0,    0,    0xc3, 0x31, 0xc0, 0xc3});
  Image.LoadsAtLinkAddress = true;
  Segment Table;
  Table.Name = ".rodata";
  Table.VA = 0x2000;
  Table.Size = Table.FileSz = 16;
  Table.Flags = SegmentFlags::Readable;
  Table.Data.resize(16);
  for (unsigned I = 0; I != 2; ++I) {
    const va_t Target = 0x1010 + I * 6;
    for (unsigned J = 0; J != 8; ++J)
      Table.Data[I * 8 + J] = static_cast<uint8_t>(Target >> (J * 8));
    Image.DataPtrRelocSlots.insert(Table.VA + I * 8);
  }
  Image.Segments.push_back(std::move(Table));
  auto Cache = createX87CallGraphCache(nullptr, nullptr, true);
  CFGBuilder Seed, Cold;
  Seed.setX87CallGraphCache(Cache);
  Access::query(Seed, Image, Image.Entry);
  const auto Warm = cachedFunction(Cache, Image);
  ASSERT_TRUE(Warm);
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Image));
  const auto Original = Cold.build(Image, Dec, Image.Entry);
  ASSERT_EQ(Original.JumpTables.size(), 1u);
  EXPECT_EQ(Original.JumpTables.front().Targets.size(), 2u);
  ASSERT_FALSE(Original.JumpTables.front().AuthenticatedTableLoads.empty());
  expectSameFunctionFacts(Original, *Warm);
}

TEST(CompleteFunctionCache, ExceptionRecordsAreCopiedFromTheFrozenImage) {
  using Access = detail::X87CallGraphCacheTestAccess;
  auto Image = x87RejectedGraphImage(Arch::X64, true);
  Image.Segments.front().Data[0] = 0x31;
  Image.Segments.front().Data[1] = 0xc0;
  Image.Segments.front().Data[0x10] = 0x31;
  Image.Segments.front().Data[0x11] = 0xc9;
  auto &EH = Image.ExceptionMetadata.Functions.front();
  EH.PersonalityName = "__gxx_personality_v0";
  EH.Diagnostics.push_back(std::string(128 * 1024, 'x'));
  EH.Dwarf.emplace();
  EH.Dwarf->Instructions.emplace_back();
  EH.Dwarf->Instructions.back().Expression = {1, 2, 3};
  EH.Itanium->TypeTable.emplace_back();
  EH.Itanium->TypeTable.back().TypeName = "test_exception_type";
  auto Cache = createX87CallGraphCache(nullptr, nullptr, true);
  CFGBuilder Seed, Cold;
  Seed.setX87CallGraphCache(Cache);
  Access::query(Seed, Image, Image.Entry);
  auto Warm = cachedFunction(Cache, Image);
  ASSERT_TRUE(Warm);
  ASSERT_TRUE(Warm->ExceptionMetadata);
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Image));
  const auto Original = Cold.build(Image, Dec, Image.Entry);
  expectSameFunctionFacts(Original, *Warm);
  EXPECT_EQ(Warm->ExceptionMetadata->Itanium->CallSites.size(), 1u);
  // The large loader-owned diagnostic is not duplicated inside the cache.
  EXPECT_LT(Access::summaryStats(Seed)[1], 128u * 1024);
  Seed.setX87CallGraphCache(nullptr);
  Cache.reset();
  Image = BinaryImage{};
  EXPECT_EQ(Warm->ExceptionMetadata->PersonalityName, "__gxx_personality_v0");
  EXPECT_EQ(Warm->ExceptionMetadata->Diagnostics.front(),
            std::string(128 * 1024, 'x'));
  EXPECT_EQ(Warm->ExceptionMetadata->Dwarf->Instructions.front().Expression,
            (std::vector<uint8_t>{1, 2, 3}));
  EXPECT_EQ(Warm->ExceptionMetadata->Itanium->TypeTable.front().TypeName,
            "test_exception_type");
}

TEST(CallRegisterBodyCache, CompleteBranchesKeepMachineAndRegisterFacts) {
  using Access = detail::X87CallGraphCacheTestAccess;
  for (BinaryFormat Format :
       {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
    SCOPED_TRACE(static_cast<unsigned>(Format));
    auto Image =
        registerBodyImage(Format, {0x85, 0xff, 0x74, 7, 0xb8, 7, 0, 0, 0, 0xeb,
                                   5, 0xb8, 9, 0, 0, 0, 0xc3});
    const std::set<va_t> Entries{0x1000, 0x1100};
    auto Cache = createX87CallGraphCache(nullptr, nullptr, true);
    CFGBuilder Seed, Cold;
    Seed.setKnownFuncEntries(&Entries);
    Cold.setKnownFuncEntries(&Entries);
    Seed.setX87CallGraphCache(Cache);
    EXPECT_EQ(Access::query(Seed, Image, Image.Entry).first,
              std::optional<int>(0));
    const auto Body = cachedRegisterBody(Cache, Image, &Entries);
    ASSERT_TRUE(Body);
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Image));
    const auto Original = Cold.build(Image, Dec, Image.Entry);
    expectSameRegisterBody(Original, *Body);
    const auto Effect = localRegisterEffect(Image, CallRegisterBodyView(*Body));
    EXPECT_EQ(Effect.Writes, GPRFamilyMask(1) << (x86reg::RAX / 8));
    EXPECT_FALSE(Effect.Unknown);
    EXPECT_FALSE(Effect.Incomplete);
    EXPECT_TRUE(Effect.Callees.empty());
    // Cached machine data still pays every x87 proof step at every budget.
    const auto Full = Access::query(Cold, Image, Image.Entry);
    const size_t Cost = limits::kMaxX87CallProofWork - Full.second;
    for (size_t Budget = 0; Budget <= Cost + 1; ++Budget)
      EXPECT_EQ(Access::query(Seed, Image, Image.Entry, Budget),
                Access::query(Cold, Image, Image.Entry, Budget));
  }
}

TEST(CallRegisterBodyCache,
     LocalStackStateAndFloatingCallResultsKeepColdBuilds) {
  using Access = detail::X87CallGraphCacheTestAccess;
  const std::vector<std::vector<uint8_t>> Local{
      {0xd9, 0xe8, 0xdd, 0xd8, 0xc3}, // fld1; fstp st0; ret
      {0xdb, 0xe3, 0xc3},             // fninit; ret
      {0xd9, 0xf7, 0xc3},             // fincstp; ret
      {0xd9, 0xe5, 0xc3},             // fxam; ret
      {0x0f, 0x6f, 0xc1, 0xc3}};      // movq mm0,mm1; ret
  for (const auto &Bytes : Local) {
    auto Image = registerBodyImage(BinaryFormat::ELF, Bytes);
    auto Cache = createX87CallGraphCache(nullptr, nullptr, true);
    CFGBuilder Seed;
    Seed.setX87CallGraphCache(Cache);
    Access::query(Seed, Image, Image.Entry);
    EXPECT_FALSE(cachedRegisterBody(Cache, Image));
    EXPECT_FALSE(cachedFunction(Cache, Image));
  }
  for (BinaryFormat Format : {BinaryFormat::ELF, BinaryFormat::COFF}) {
    SCOPED_TRACE(static_cast<int>(Format));
    auto Image = registerBodyImage(Format, {0xe8, 0xfb, 0, 0, 0, 0xc3},
                                   {0xd9, 0xe8, 0xc3});
    const bool Owned = ownsX87(Image);
    auto Cache = createX87CallGraphCache(nullptr, nullptr, true);
    CFGBuilder Seed;
    Seed.setX87CallGraphCache(Cache);
    EXPECT_EQ(Access::query(Seed, Image, Image.Entry).first,
              Owned ? std::nullopt : std::optional<int>(-1));
    EXPECT_FALSE(cachedRegisterBody(Cache, Image));
    EXPECT_FALSE(cachedFunction(Cache, Image));
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Image));
    const auto Cold = Seed.build(Image, Dec, Image.Entry);
    EXPECT_EQ(hasX87CallDefinition(Cold), !Owned);
    if (Owned)
      expectOwnedCallTransport(Cold);
  }
}

TEST(CallRegisterBodyCache, NoReturnDecisionsMustAgreeAtOrdinaryDepth) {
  using Access = detail::X87CallGraphCacheTestAccess;
  struct Prover : NoReturnCalleeProver {
    bool Different;
    explicit Prover(bool Value) : Different(Value) {}
    bool mayNeverReturn(va_t Target) const override { return Target == 0x1100; }
    bool neverReturns(va_t, unsigned Depth) const override {
      return Different && Depth == 0;
    }
  };
  for (bool Different : {false, true}) {
    SCOPED_TRACE(Different);
    Prover Proof(Different);
    auto Image = registerBodyImage(
        BinaryFormat::ELF, {0xe8, 0xfb, 0, 0, 0, 0xb8, 7, 0, 0, 0, 0xc3});
    auto Cache = createX87CallGraphCache(&Proof, nullptr, true);
    CFGBuilder Seed;
    Seed.setNoReturnCalleeProver(&Proof);
    Seed.setX87CallGraphCache(Cache);
    Access::query(Seed, Image, Image.Entry);
    EXPECT_GT(Access::summaryStats(Seed)[0], 0u);
    const auto Body = cachedRegisterBody(Cache, Image, nullptr, &Proof);
    EXPECT_EQ(bool(Body), !Different);
    EXPECT_EQ(bool(cachedFunction(Cache, Image, nullptr, &Proof)), !Different);
    if (Body) {
      Decoder Dec;
      ASSERT_TRUE(Dec.init(Image));
      expectSameRegisterBody(Seed.build(Image, Dec, Image.Entry), *Body);
    }
  }
}

TEST(CallRegisterBodyCache, LoadedImportSlotsRetainFloatingReturnDefinitions) {
  using Access = detail::X87CallGraphCacheTestAccess;
  auto Image =
      registerBodyImage(BinaryFormat::ELF, {0x48, 0x8b, 0x05, 0xf9, 1, 0,
                                            0, // mov rax,[rip+0x1f9] -> 0x1200
                                            0xff, 0xd0, 0xc3}); // call rax; ret
  Segment Slot;
  Slot.Name = ".got";
  Slot.VA = 0x1200;
  Slot.Size = Slot.FileSz = 8;
  Slot.Data.resize(8);
  Slot.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Image.Segments.push_back(std::move(Slot));
  Import Imported;
  // This import has an explicit long-double return contract. A recognized
  // routine name alone does not supply the x87 result proof.
  Imported.Name = "strtold";
  Imported.IATAddr = 0x1200;
  Image.Imports.push_back(std::move(Imported));
  auto Cache = createX87CallGraphCache(nullptr, nullptr, true);
  CFGBuilder Seed;
  Seed.setX87CallGraphCache(Cache);
  EXPECT_EQ(Access::query(Seed, Image, Image.Entry).first,
            std::optional<int>(-1));
  EXPECT_FALSE(cachedRegisterBody(Cache, Image));
  EXPECT_FALSE(cachedFunction(Cache, Image));
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Image));
  EXPECT_TRUE(hasX87CallDefinition(Seed.build(Image, Dec, Image.Entry)));
}

TEST(CallRegisterBodyCache, RawWrappersCannotBypassX87ArgumentBinding) {
  using Access = detail::X87CallGraphCacheTestAccess;
  for (unsigned Kind = 0; Kind < 4; ++Kind) {
    auto Image = Kind == 0 ? x87ConsumingCallImage(BinaryFormat::ELF)
                           : x87CRTCallImage("_CIsqrt", 0);
    if (Kind == 0) {
      wrapX87Consumer(Image, false);
      Image.Entry = 0x1100;
    } else if (Kind == 2) {
      Image.Imports[0].Module = "unrelated.dll";
    } else if (Kind == 3) {
      Image.Arch = Arch::X64;
      Image.Bits = Bitness::Bits64;
      const uint32_t Displacement = 0x2000 - 0x1106;
      for (unsigned K = 0; K < 4; ++K)
        Image.Segments[0].Data[0x102 + K] = uint8_t(Displacement >> (8 * K));
      Image.Segments[1].Size = Image.Segments[1].FileSz = 8;
      Image.Segments[1].Data.resize(8);
    }
    const std::set<va_t> Entries{0x1000, 0x1100, 0x1200};
    auto Cache = createX87CallGraphCache(nullptr, nullptr, true);
    CFGBuilder Seed;
    Seed.setKnownFuncEntries(&Entries);
    Seed.setX87CallGraphCache(Cache);
    Access::query(Seed, Image, Image.Entry);
    // Register-body replay is currently x64-only. The i386 cases exercise
    // the ordinary path, and the x64 case must reject its retained raw body.
    EXPECT_EQ(Access::summaryStats(Seed)[0] > 0, Kind == 3) << Kind;
    EXPECT_FALSE(cachedRegisterBody(Cache, Image, &Entries)) << Kind;
    EXPECT_FALSE(cachedFunction(Cache, Image, &Entries)) << Kind;
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Image));
    const auto Low = Seed.build(Image, Dec, Image.Entry);
    EXPECT_EQ(Low.hasCompleteInstructionLift(), Kind == 0);
    EXPECT_EQ(Low.ConsumesX87Argument, Kind == 0);
  }
}

TEST(CallRegisterBodyCache, ReplayReentryAndExceptionsKeepAnOrdinaryFallback) {
  using Access = detail::X87CallGraphCacheTestAccess;
  struct Prover : NoReturnCalleeProver {
    std::function<void()> OnReplay;
    bool mayNeverReturn(va_t Target) const override { return Target == 0x1100; }
    bool neverReturns(va_t, unsigned Depth) const override {
      if (Depth == 0 && OnReplay)
        OnReplay();
      return false;
    }
  } Proof;
  auto Image =
      registerBodyImage(BinaryFormat::ELF, {0xe8, 0xfb, 0, 0, 0, 0xc3});
  auto Cache = createX87CallGraphCache(&Proof, nullptr, true);
  CFGBuilder Seed;
  Seed.setX87CallGraphCache(Cache);
  Seed.setNoReturnCalleeProver(&Proof);
  Access::query(Seed, Image, Image.Entry);
  unsigned Reentries = 0;
  Proof.OnReplay = [&] {
    ++Reentries;
    EXPECT_FALSE(cachedRegisterBody(Cache, Image, nullptr, &Proof));
    EXPECT_FALSE(cachedFunction(Cache, Image, nullptr, &Proof));
  };
  EXPECT_TRUE(cachedRegisterBody(Cache, Image, nullptr, &Proof));
  EXPECT_GT(Reentries, 0u);
  Proof.OnReplay = [] { throw std::runtime_error("replay failed"); };
  EXPECT_THROW(cachedRegisterBody(Cache, Image, nullptr, &Proof),
               std::runtime_error);
  EXPECT_THROW(cachedFunction(Cache, Image, nullptr, &Proof),
               std::runtime_error);
  Proof.OnReplay = {};
  EXPECT_TRUE(cachedRegisterBody(Cache, Image, nullptr, &Proof));
}

TEST(CallRegisterBodyCache, FixedCallArgumentsAreBoundAtTheConsumer) {
  using Access = detail::X87CallGraphCacheTestAccess;
  auto Image =
      registerBodyImage(BinaryFormat::ELF, {0xe8, 0xfb, 0, 0, 0, 0xc3});
  auto Cache = createX87CallGraphCache(nullptr, nullptr, true);
  CFGBuilder Seed;
  Seed.setX87CallGraphCache(Cache);
  Access::query(Seed, Image, Image.Entry);
  const auto Body = cachedRegisterBody(Cache, Image);
  ASSERT_TRUE(Body);
  std::map<va_t, GPRReadWidths> Fixed;
  Fixed[0x1000][x86reg::RDI / 8] = 4;
  const auto Plain = localRegisterEffect(Image, CallRegisterBodyView(*Body));
  const auto Bound =
      localRegisterEffect(Image, CallRegisterBodyView(*Body), nullptr, &Fixed);
  EXPECT_FALSE(Plain.CallsFixedContract);
  EXPECT_EQ(Plain.Callees, (std::set<va_t>{0x1100}));
  EXPECT_TRUE(Bound.CallsFixedContract);
  EXPECT_TRUE(Bound.Callees.empty());
  unsigned BoundSteps = 0;
  for (const auto &Block : Bound.Blocks)
    for (const auto &Step : Block.Steps)
      if (Step.FixedArguments) {
        ++BoundSteps;
        EXPECT_EQ(*Step.FixedArguments, Fixed.at(0x1000));
      }
  EXPECT_EQ(BoundSteps, 1u);
}

TEST(CallRegisterBodyCache, ExactContextAndExplicitOptInAreRequired) {
  using Access = detail::X87CallGraphCacheTestAccess;
  auto Image = registerBodyImage(BinaryFormat::ELF, {0xb8, 7, 0, 0, 0, 0xc3});
  auto Other = Image;
  const std::set<va_t> Entries{0x1000, 0x1100}, Changed{0x1000, 0x1001, 0x1100};
  for (bool Enabled : {false, true}) {
    auto Cache = createX87CallGraphCache(nullptr, nullptr, Enabled);
    CFGBuilder Seed;
    Seed.setKnownFuncEntries(&Entries);
    Seed.setX87CallGraphCache(Cache);
    Access::query(Seed, Image, Image.Entry);
    EXPECT_EQ(bool(cachedRegisterBody(Cache, Image, &Entries)), Enabled);
    EXPECT_EQ(bool(cachedFunction(Cache, Image, &Entries)), Enabled);
    EXPECT_FALSE(cachedRegisterBody(Cache, Image, &Changed));
    EXPECT_FALSE(cachedFunction(Cache, Image, &Changed));
    EXPECT_FALSE(cachedRegisterBody(Cache, Image));
    EXPECT_FALSE(cachedFunction(Cache, Image));
    EXPECT_FALSE(cachedRegisterBody(Cache, Other, &Entries));
    EXPECT_FALSE(cachedFunction(Cache, Other, &Entries));
  }
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    auto Cache = createX87CallGraphCache(nullptr, nullptr, true);
    CFGBuilder Seed;
    const std::set<va_t> Empty;
    if (Mutation == 0)
      Seed.setProtectedJumpTableRelocationSlots(&Empty);
    else if (Mutation == 1)
      Seed.setUnsafeJumpTableBranches(&Empty);
    else if (Mutation == 2)
      Seed.setNoReturnCalleeProver(nullptr, 1);
    else
      Image.Mode = InstructionMode::Default;
    Seed.setX87CallGraphCache(Cache);
    Access::query(Seed, Image, Image.Entry);
    if (Mutation < 3) {
      EXPECT_FALSE(cachedRegisterBody(Cache, Image));
      EXPECT_FALSE(cachedFunction(Cache, Image));
    } else {
      // The caller must retire the cache on byte/metadata mutation. A fresh
      // operation has no receipt from an earlier image even at the same VA.
      Cache = createX87CallGraphCache(nullptr, nullptr, true);
      EXPECT_FALSE(cachedRegisterBody(Cache, Image));
      EXPECT_FALSE(cachedFunction(Cache, Image));
    }
  }
}

TEST(CallRegisterBodyCache, ExceptionEdgesRemainAvailableToSummaryReaders) {
  using Access = detail::X87CallGraphCacheTestAccess;
  auto Image = x87RejectedGraphImage(Arch::X64, true);
  // Replace the two FLD1 instructions with GPR-only instructions. The x87
  // projection still refuses exceptional edges, while the complete machine
  // graph remains useful to ordinary argument/register analysis.
  Image.Segments.front().Data[0] = 0x31;
  Image.Segments.front().Data[1] = 0xc0;
  Image.Segments.front().Data[0x10] = 0x31;
  Image.Segments.front().Data[0x11] = 0xc9;
  auto Cache = createX87CallGraphCache(nullptr, nullptr, true);
  CFGBuilder Seed;
  Seed.setX87CallGraphCache(Cache);
  EXPECT_FALSE(Access::query(Seed, Image, Image.Entry).first);
  const auto Body = cachedRegisterBody(Cache, Image);
  ASSERT_TRUE(Body);
  EXPECT_TRUE(llvm::any_of(Body->Blocks, [](const LowBlock &Block) {
    return !Block.ExceptionalSuccs.empty();
  }));
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Image));
  expectSameRegisterBody(Seed.build(Image, Dec, Image.Entry), *Body);
}

TEST(CallRegisterBodyCache, IncompleteBodiesServeOnlyCallEffects) {
  using Access = detail::X87CallGraphCacheTestAccess;
  // A truncated instruction, and a jump out of the image.
  for (const std::vector<uint8_t> Bytes :
       {std::vector<uint8_t>{0x0f},
        std::vector<uint8_t>{0xe9, 0xfb, 0x7f, 0, 0}}) {
    auto Image = x87ExamineImage(Arch::X64, BinaryFormat::ELF, Bytes);
    auto Cache = createX87CallGraphCache(nullptr, nullptr, true);
    CFGBuilder Seed;
    Seed.setX87CallGraphCache(Cache);
    Access::query(Seed, Image, Image.Entry);
    EXPECT_EQ(Access::summaryStats(Seed)[0], 1u);
    // A caller's call effects read the retained body as they read a fresh
    // build: incomplete, with the same effect.
    const auto Body = cachedRegisterBody(Cache, Image);
    ASSERT_TRUE(Body);
    EXPECT_FALSE(Body->Complete);
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Image));
    CFGBuilder Fresh;
    const LowFunc Built = Fresh.build(Image, Dec, Image.Entry);
    ASSERT_FALSE(Built.hasCompleteLiftCoverage());
    const auto Kept = localRegisterEffect(Image, CallRegisterBodyView(*Body));
    const auto Cold = localRegisterEffect(Image, CallRegisterBodyView(Built));
    EXPECT_EQ(Kept.Incomplete, Cold.Incomplete);
    EXPECT_EQ(Kept.Unknown, Cold.Unknown);
    EXPECT_EQ(Kept.Writes, Cold.Writes);
    EXPECT_EQ(Kept.Callees, Cold.Callees);
    EXPECT_EQ(Kept.Blocks.size(), Cold.Blocks.size());
    // A selected function's own build is never one of them.
    EXPECT_FALSE(cachedFunction(Cache, Image));
  }
}

TEST(CallRegisterBodyCache, UnknownCallEffectsRemainUnknownAndCallbacksOptIn) {
  using Access = detail::X87CallGraphCacheTestAccess;
  auto Image =
      registerBodyImage(BinaryFormat::ELF, {0xe8, 0xfb, 0, 0, 0, 0xc3}, {0x0f});
  auto Cache = createX87CallGraphCache(nullptr, nullptr, true);
  CFGBuilder Seed;
  Seed.setX87CallGraphCache(Cache);
  EXPECT_FALSE(Access::query(Seed, Image, Image.Entry).first);
  const auto Body = cachedRegisterBody(Cache, Image);
  ASSERT_TRUE(Body);
  EXPECT_FALSE(Access::query(Seed, Image, Image.Entry).first);
  const auto Effect = localRegisterEffect(Image, CallRegisterBodyView(*Body));
  EXPECT_EQ(Effect.Callees, (std::set<va_t>{0x1100}));
  struct Prover : NoReturnCalleeProver {
    bool neverReturns(va_t, unsigned) const override { return false; }
  } Unregistered;
  auto OtherCache = createX87CallGraphCache(nullptr, nullptr, true);
  CFGBuilder Other;
  Other.setX87CallGraphCache(OtherCache);
  Other.setNoReturnCalleeProver(&Unregistered);
  Access::query(Other, Image, Image.Entry);
  EXPECT_FALSE(cachedRegisterBody(OtherCache, Image, nullptr, &Unregistered));
  EXPECT_EQ(Access::summaryStats(Other)[0], 0u);
}

TEST(CallRegisterBodyCache, BlockPayloadEnforcesTheByteLimitBeforeEntryLimit) {
  using Access = detail::X87CallGraphCacheTestAccess;
  constexpr unsigned Count = 40, Instructions = 4096, Stride = 0x8000;
  auto Image = x87ExamineImage(Arch::X64, BinaryFormat::ELF,
                               std::vector<uint8_t>(Count * Stride, 0xcc));
  Image.Symbols.clear();
  Image.KnownCodeRanges.clear();
  std::set<va_t> Entries;
  for (unsigned I = 0; I != Count; ++I) {
    const va_t Entry = 0x1000 + I * Stride;
    for (unsigned J = 0; J != Instructions; ++J) {
      auto It = Image.Segments.front().Data.begin() + I * Stride + J * 5;
      const std::array<uint8_t, 5> Move{0xb8, 7, 0, 0, 0};
      std::copy(Move.begin(), Move.end(), It);
    }
    Image.Segments.front().Data[I * Stride + Instructions * 5] = 0xc3;
    Image.Symbols.push_back(Symbol::makeFunc(Entry, Instructions * 5 + 1));
    Image.KnownCodeRanges.emplace_back(Entry, Entry + Instructions * 5 + 1);
    Entries.insert(Entry);
  }
  auto Cache = createX87CallGraphCache(nullptr, nullptr, true);
  CFGBuilder Seed;
  Seed.setKnownFuncEntries(&Entries);
  Seed.setX87CallGraphCache(Cache);
  size_t Filled = 0, BodiesThatFit = 0;
  for (va_t Entry : Entries) {
    Access::query(Seed, Image, Entry);
    if (++Filled == 1) {
      const auto Body = cachedRegisterBody(Cache, Image, &Entries);
      ASSERT_TRUE(Body);
      size_t LiveBytes = Body->Blocks.size() * sizeof(LowBlock);
      for (const auto &Block : Body->Blocks)
        LiveBytes += Block.Ops.size() * sizeof(LowOp) +
                     Block.InstructionBoundaries.size() *
                         sizeof(LowInstructionBoundary) +
                     (Block.Succs.size() + Block.Preds.size()) * sizeof(int);
      ASSERT_GT(LiveBytes, 0u);
      // Identical straight-line bodies fit together after reserving 4 MiB
      // for their contexts/metadata. Construction slack must not evict them.
      BodiesThatFit = 28u * 1024 * 1024 / LiveBytes;
      ASSERT_GT(BodiesThatFit, 1u);
      ASSERT_LT(BodiesThatFit, Count);
    }
    if (Filled == BodiesThatFit) {
      EXPECT_EQ(Access::summaryStats(Seed)[0], Filled);
      EXPECT_TRUE(cachedRegisterBody(Cache, Image, &Entries));
      EXPECT_TRUE(cachedFunction(Cache, Image, &Entries));
    }
    EXPECT_LE(Access::summaryStats(Seed)[1], 32u * 1024 * 1024);
  }
  EXPECT_GT(Access::summaryStats(Seed)[0], 0u);
  EXPECT_LT(Access::summaryStats(Seed)[0], Count);
  EXPECT_FALSE(cachedRegisterBody(Cache, Image, &Entries));
  EXPECT_FALSE(cachedFunction(Cache, Image, &Entries));
  EXPECT_TRUE(CFGBuilder::cachedCallRegisterBody(
      Cache, Image, *Entries.rbegin(), &Entries, nullptr, nullptr, nullptr,
      nullptr));
}

TEST(CallRegisterBodyCache, ParallelReadersAndEvictionKeepBorrowedBodiesAlive) {
  using Access = detail::X87CallGraphCacheTestAccess;
  auto Image = x87ExamineImage(Arch::X64, BinaryFormat::ELF,
                               std::vector<uint8_t>(160 * 16, 0xcc));
  Image.Symbols.clear();
  Image.KnownCodeRanges.clear();
  std::set<va_t> Entries;
  for (unsigned I = 0; I != 160; ++I) {
    const va_t Address = 0x1000 + I * 16;
    Image.Segments.front().Data[I * 16] = 0xc3;
    Image.Symbols.push_back(Symbol::makeFunc(Address, 1));
    Image.KnownCodeRanges.emplace_back(Address, Address + 1);
    Entries.insert(Address);
  }
  auto Cache = createX87CallGraphCache(nullptr, nullptr, true);
  CFGBuilder Seed;
  Seed.setKnownFuncEntries(&Entries);
  Seed.setX87CallGraphCache(Cache);
  Access::query(Seed, Image, Image.Entry);
  const auto First = cachedRegisterBody(Cache, Image, &Entries);
  ASSERT_TRUE(First);
  std::barrier Start(4);
  std::vector<std::future<bool>> Workers;
  for (unsigned I = 0; I != 4; ++I)
    Workers.push_back(std::async(std::launch::async, [&] {
      Start.arrive_and_wait();
      for (unsigned J = 0; J != 16; ++J)
        if (cachedRegisterBody(Cache, Image, &Entries) != First ||
            !cachedFunction(Cache, Image, &Entries))
          return false;
      return true;
    }));
  for (auto &Worker : Workers)
    EXPECT_TRUE(Worker.get());
  for (va_t Entry : Entries)
    Access::query(Seed, Image, Entry);
  EXPECT_FALSE(cachedRegisterBody(Cache, Image, &Entries));
  EXPECT_FALSE(cachedFunction(Cache, Image, &Entries));
  EXPECT_LE(Access::summaryStats(Seed)[0], 128u);
  EXPECT_LE(Access::summaryStats(Seed)[1], 32u * 1024 * 1024);
  ASSERT_EQ(First->Blocks.size(), 1u);
  EXPECT_EQ(First->Blocks.front().StartAddr, Image.Entry);
}

TEST(X87CallGraphCache, ImmutableProjectionRejectionsReplayBudgetsExactly) {
  using Access = detail::X87CallGraphCacheTestAccess;
  for (Arch Architecture : {Arch::X86, Arch::X64})
    for (bool Exceptional : {false, true}) {
      SCOPED_TRACE(static_cast<int>(Architecture));
      SCOPED_TRACE(Exceptional);
      BinaryImage Image = x87RejectedGraphImage(Architecture, Exceptional);
      // Preserve an unresolved dispatch instead of granting it an indirect
      // tail-call classification. This is an ordinary CFG policy input.
      const std::set<va_t> Unsafe{Image.Entry};
      Decoder Dec;
      ASSERT_TRUE(Dec.init(Image));
      CFGBuilder Shape;
      Shape.setUnsafeJumpTableBranches(&Unsafe);
      const LowFunc Low = Shape.build(Image, Dec, Image.Entry);
      ASSERT_TRUE(Low.hasCompleteInstructionLift());
      ASSERT_TRUE(Low.TruncatedPathAddresses.empty());
      ASSERT_FALSE(Low.Blocks.empty());
      if (Exceptional) {
        ASSERT_FALSE(Low.Blocks.front().ExceptionalSuccs.empty());
      } else {
        ASSERT_EQ(Low.Blocks.size(), 1u);
        ASSERT_TRUE(Low.Blocks.front().Succs.empty());
        ASSERT_FALSE(Low.Blocks.front().Ops.empty());
        ASSERT_EQ(Low.Blocks.front().Ops.back().Opcode, NdOp::INDIR_BR);
      }

      auto Cache = createX87CallGraphCache();
      CFGBuilder Seed;
      Seed.setUnsafeJumpTableBranches(&Unsafe);
      Seed.setX87CallGraphCache(Cache);
      const auto Rejected = Access::query(Seed, Image, Image.Entry);
      EXPECT_FALSE(Rejected.first);
      EXPECT_EQ(Rejected.second, limits::kMaxX87CallProofWork - 1);
      ASSERT_EQ(Access::stats(Seed)[1], 1u);
      for (size_t Budget :
           {size_t(0), size_t(1), size_t(2), size_t(3), size_t(8), size_t(64),
            size_t(limits::kMaxX87CallProofWork)}) {
        SCOPED_TRACE(Budget);
        CFGBuilder Cold, Warm;
        Cold.setUnsafeJumpTableBranches(&Unsafe);
        Warm.setUnsafeJumpTableBranches(&Unsafe);
        Warm.setX87CallGraphCache(Cache);
        const auto Before = Access::stats(Warm);
        const auto Actual = Access::query(Warm, Image, Image.Entry, Budget);
        EXPECT_EQ(Actual, Access::query(Cold, Image, Image.Entry, Budget));
        EXPECT_FALSE(Actual.first);
        EXPECT_EQ(Actual.second, Budget ? Budget - 1 : 0);
        EXPECT_EQ(Access::stats(Warm)[0], Before[0] + (Budget != 0));
        EXPECT_EQ(Access::stats(Warm)[1], 1u);
      }
    }
}

TEST(X87CallGraphCache, OversizedGraphsAreRetainedAsRefusals) {
  // A body larger than any proof may walk is refused for its size, a fact of
  // its complete CFG: the next builder reads the refusal, budget for budget,
  // instead of lifting the body again (Wine's 32-bit printf engine takes
  // seconds in its jump tables).
  using Access = detail::X87CallGraphCacheTestAccess;
  BinaryImage Image;
  Image.Arch = Arch::X86;
  Image.Bits = Bitness::Bits32;
  Image.Format = BinaryFormat::ELF;
  Image.Entry = 0x1000;
  Segment Text;
  Text.Name = ".text";
  Text.VA = Image.Entry;
  Text.Data.assign(60000, 0x40); // inc eax
  Text.Data.push_back(0xc3);     // ret
  Text.Size = Text.Data.size();
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Image.Segments.push_back(Text);
  Image.Symbols.push_back(Symbol::makeFunc(Image.Entry, Text.Size));
  Image.KnownCodeRanges.emplace_back(Image.Entry, Image.Entry + Text.Size);

  auto Cache = createX87CallGraphCache();
  CFGBuilder Seed;
  Seed.setX87CallGraphCache(Cache);
  EXPECT_FALSE(Access::query(Seed, Image, Image.Entry).first);
  ASSERT_EQ(Access::stats(Seed)[1], 1u);
  for (size_t Budget : {size_t(0), size_t(limits::kMaxX87CallProofWork)}) {
    SCOPED_TRACE(Budget);
    CFGBuilder Cold, Warm;
    Warm.setX87CallGraphCache(Cache);
    const auto Before = Access::stats(Warm);
    const auto Actual = Access::query(Warm, Image, Image.Entry, Budget);
    EXPECT_EQ(Actual, Access::query(Cold, Image, Image.Entry, Budget));
    EXPECT_FALSE(Actual.first);
    EXPECT_EQ(Access::stats(Warm)[0], Before[0] + (Budget != 0));
    EXPECT_EQ(Access::stats(Warm)[1], 1u);
  }
}

TEST(X87CallGraphCache, RejectionMarkersCannotSurviveChangedBuildContexts) {
  using Access = detail::X87CallGraphCacheTestAccess;
  for (bool Exceptional : {false, true})
    for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
      SCOPED_TRACE(Exceptional);
      SCOPED_TRACE(Mutation);
      BinaryImage Image = x87RejectedGraphImage(Arch::X86, Exceptional);
      BinaryImage Valid = x87CallLoopImage(Arch::X86, BinaryFormat::ELF);
      std::set<va_t> Entries{0x1200}, Protected, Unsafe{0x1200};
      unsigned Depth = 0;
      auto Configure = [&](CFGBuilder &Builder) {
        Builder.setKnownFuncEntries(&Entries);
        Builder.setProtectedJumpTableRelocationSlots(&Protected);
        Builder.setUnsafeJumpTableBranches(&Unsafe);
        Builder.setNoReturnCalleeProver(nullptr, Depth);
      };
      auto Cache = createX87CallGraphCache();
      CFGBuilder Seed;
      Configure(Seed);
      Seed.setX87CallGraphCache(Cache);
      ASSERT_FALSE(Access::query(Seed, Image, Image.Entry).first);
      ASSERT_EQ(Access::stats(Seed)[1], 1u);
      const auto Before = Access::stats(Seed);
      const BinaryImage *CurrentImage = &Image;
      if (Mutation == 0)
        Entries.insert(0x1218);
      else if (Mutation == 1)
        Protected.insert(0x1218);
      else if (Mutation == 2)
        Unsafe.insert(0x1218);
      else if (Mutation == 3)
        Depth = 1;
      else
        CurrentImage = &Valid;
      CFGBuilder Cold, Warm;
      Configure(Cold);
      Configure(Warm);
      Warm.setX87CallGraphCache(Cache);
      const auto Actual = Access::query(Warm, *CurrentImage, 0x1200);
      EXPECT_EQ(Actual, Access::query(Cold, *CurrentImage, 0x1200));
      EXPECT_EQ(Actual.first,
                Mutation == 4 ? std::optional<int>(-1) : std::nullopt);
      EXPECT_EQ(Access::stats(Warm)[0], Before[0]);
    }
}

TEST(X87CallGraphCache, IncompleteInstructionGraphsAreRetainedOnlyAsRefusals) {
  using Access = detail::X87CallGraphCacheTestAccess;
  BinaryImage Image = x87CallLoopImage(Arch::X86, BinaryFormat::ELF);
  // The executable section ends halfway through FLD1. Ownership of the entry
  // does not turn a truncated instruction stream into a complete local graph:
  // the cache keeps only the refusal, which the next request answers from.
  Image.Segments[0].Size = 0x201;
  Image.Segments[0].Data.resize(0x201);
  auto Cache = createX87CallGraphCache();
  for (unsigned I = 0; I != 2; ++I) {
    CFGBuilder Builder;
    Builder.setX87CallGraphCache(Cache);
    EXPECT_FALSE(Access::query(Builder, Image, 0x1200).first);
    EXPECT_EQ(Access::stats(Builder)[0], I);
    EXPECT_EQ(Access::stats(Builder)[1], 1u);
  }
}

TEST(X87CallGraphCache, ConcurrentBuildersKeepIndependentProofState) {
  using Access = detail::X87CallGraphCacheTestAccess;
  BinaryImage Image = x87CallLoopImage(Arch::X86, BinaryFormat::ELF);
  const std::set<va_t> Entries{0x1000, 0x1100, 0x1200};
  CFGBuilder Cold;
  Cold.setKnownFuncEntries(&Entries);
  const auto Expected = Access::query(Cold, Image, 0x1100);
  ASSERT_EQ(Expected.first, std::optional<int>(-1));
  auto Cache = createX87CallGraphCache();
  std::barrier Start(4);
  std::vector<std::future<bool>> Workers;
  for (unsigned I = 0; I != 4; ++I)
    Workers.push_back(std::async(std::launch::async, [&] {
      Start.arrive_and_wait();
      for (unsigned J = 0; J != 8; ++J) {
        CFGBuilder Builder;
        Builder.setKnownFuncEntries(&Entries);
        Builder.setX87CallGraphCache(Cache);
        if (Access::query(Builder, Image, 0x1100) != Expected)
          return false;
      }
      return true;
    }));
  for (auto &Worker : Workers)
    EXPECT_TRUE(Worker.get());
  CFGBuilder Observer;
  Observer.setX87CallGraphCache(Cache);
  const auto Stats = Access::stats(Observer);
  EXPECT_GT(Stats[0], 0u);
  EXPECT_LE(Stats[1], 128u);
  EXPECT_LE(Stats[2], 8u * 1024 * 1024);
}

namespace {
// This callback holds the leader at a real CFG construction boundary. Tests
// wait for a registered follower, not for a guessed wall-clock overlap.
class GatedX87Prover final : public NoReturnCalleeProver {
  mutable std::mutex Mutex;
  mutable std::condition_variable Changed;
  mutable size_t Calls = 0;
  bool Released = false;
  bool ThrowFirst;

public:
  explicit GatedX87Prover(bool Throw = false) : ThrowFirst(Throw) {}
  bool mayNeverReturn(va_t) const override { return true; }
  bool neverReturns(va_t, unsigned) const override {
    std::unique_lock<std::mutex> Lock(Mutex);
    const size_t Number = ++Calls;
    Changed.notify_all();
    Changed.wait(Lock, [&] { return Released; });
    if (ThrowFirst && Number == 1)
      throw std::runtime_error("controlled x87 graph build failure");
    return false;
  }
  bool waitForCalls(size_t Count) const {
    std::unique_lock<std::mutex> Lock(Mutex);
    return Changed.wait_for(Lock, std::chrono::seconds(5),
                            [&] { return Calls >= Count; });
  }
  void release() {
    std::lock_guard<std::mutex> Lock(Mutex);
    Released = true;
    Changed.notify_all();
  }
  size_t calls() const {
    std::lock_guard<std::mutex> Lock(Mutex);
    return Calls;
  }
};

class ReturningX87Prover final : public NoReturnCalleeProver {
public:
  bool mayNeverReturn(va_t) const override { return true; }
  bool neverReturns(va_t, unsigned) const override { return false; }
};

struct ScopedX87Threads {
  unsigned Previous = workerThreadOverride().load();
  ScopedX87Threads() { setWorkerThreadCount(4); }
  ~ScopedX87Threads() { setWorkerThreadCount(Previous); }
};

class GatedIndependentX87Prover final : public NoReturnCalleeProver {
  mutable std::mutex Mutex;
  mutable std::condition_variable Changed;
  mutable std::set<va_t> Entered;
  bool Released = false;
  bool Throw;

public:
  explicit GatedIndependentX87Prover(bool Throw) : Throw(Throw) {}
  bool mayNeverReturn(va_t Target) const override {
    return Target == 0x6000 || Target == 0x6020;
  }
  bool neverReturns(va_t Target, unsigned) const override {
    std::unique_lock<std::mutex> Lock(Mutex);
    Entered.insert(Target);
    Changed.notify_all();
    Changed.wait(Lock, [&] { return Released; });
    if (Throw && Target == 0x6000)
      throw std::runtime_error("independent floating proof failure");
    return false;
  }
  bool waitForBoth() const {
    std::unique_lock<std::mutex> Lock(Mutex);
    return Changed.wait_for(Lock, std::chrono::seconds(5),
                            [&] { return Entered.size() == 2; });
  }
  void release() {
    std::lock_guard<std::mutex> Lock(Mutex);
    Released = true;
    Changed.notify_all();
  }
};

BinaryImage independentFloatingCalls(Arch Architecture, BinaryFormat Format) {
  auto Image = x87ExamineImage(Architecture, Format, {});
  auto &Text = Image.Segments.front();
  Text.Size = Text.FileSz = 0x5100;
  Text.Data.assign(Text.Size, 0xcc);
  Image.Symbols.clear();
  Image.KnownCodeRanges.clear();
  auto call = [&](va_t From, va_t To) {
    const size_t Offset = From - Text.VA;
    Text.Data[Offset] = 0xe8;
    const auto Relative = static_cast<uint32_t>(To - From - 5);
    for (unsigned I = 0; I != 4; ++I)
      Text.Data[Offset + 1 + I] = static_cast<uint8_t>(Relative >> (8 * I));
  };
  call(0x1000, 0x2000);
  Text.Data[5] = 0xdd; // discard the first binary80 result
  Text.Data[6] = 0xd8;
  call(0x1007, 0x3000);
  Text.Data[12] = 0xc3;
  Image.Symbols.push_back(Symbol::makeFunc(0x1000, 13));
  for (const auto [Entry, Leaf] :
       {std::pair{0x2000, 0x6000}, std::pair{0x3000, 0x6020}}) {
    const size_t Offset = Entry - Text.VA;
    // Real decoded bodies exceed the parallel admission threshold.
    std::fill_n(Text.Data.begin() + Offset, 0x900, 0x90);
    call(Entry, Leaf);
    Text.Data[Offset + 0x8ff] = 0xc3;
    Image.Symbols.push_back(Symbol::makeFunc(Entry, 0x900));
    Text.Data[Leaf - Text.VA] = 0xd9;
    Text.Data[Leaf - Text.VA + 1] = 0xe8; // fld1; ret
    Text.Data[Leaf - Text.VA + 2] = 0xc3;
    Image.Symbols.push_back(Symbol::makeFunc(Leaf, 3));
  }
  return Image;
}
} // namespace

TEST(X87CallGraphCache, IndependentCallProofsOverlapAndJoinBeforePublication) {
  ScopedX87Threads Threads;
  for (Arch Architecture : {Arch::X86, Arch::X64})
    for (BinaryFormat Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (bool Throw : {false, true}) {
        SCOPED_TRACE(static_cast<int>(Architecture));
        SCOPED_TRACE(static_cast<int>(Format));
        SCOPED_TRACE(Throw);
        const auto Image = independentFloatingCalls(Architecture, Format);
        const std::set<va_t> Entries{0x1000, 0x2000, 0x3000, 0x6000, 0x6020};
        GatedIndependentX87Prover Prover(Throw);
        auto Cache = createX87CallGraphCache(&Prover, nullptr);
        auto Build = std::async(std::launch::async, [&] {
          Decoder Dec;
          if (!Dec.init(Image))
            throw std::runtime_error("test decoder failed");
          CFGBuilder Builder;
          Builder.setKnownFuncEntries(&Entries);
          Builder.setNoReturnCalleeProver(&Prover);
          Builder.setX87CallGraphCache(Cache);
          Builder.setX87CallProofThreads(4);
          return Builder.build(Image, Dec, Image.Entry);
        });
        const bool Overlapped = Prover.waitForBoth();
        Prover.release(); // Always unblock and join, including on failure.
        EXPECT_TRUE(Overlapped);
        if (Throw) {
          EXPECT_THROW(Build.get(), std::runtime_error);
          continue;
        }
        const LowFunc Parallel = Build.get();
        ASSERT_TRUE(Parallel.hasCompleteInstructionLift());
        Decoder Dec;
        ASSERT_TRUE(Dec.init(Image));
        CFGBuilder SerialBuilder;
        SerialBuilder.setKnownFuncEntries(&Entries);
        const LowFunc Serial = SerialBuilder.build(Image, Dec, Image.Entry);
        std::string ParallelDump, SerialDump;
        llvm::raw_string_ostream ParallelOS(ParallelDump), SerialOS(SerialDump);
        Pipeline::dumpLowIR({Parallel}, ParallelOS);
        Pipeline::dumpLowIR({Serial}, SerialOS);
        EXPECT_EQ(ParallelDump, SerialDump);
      }
}

TEST(X87CallGraphCache, ParallelQuotaDoesNotScheduleUnregisteredCallbacks) {
  ScopedX87Threads Threads;
  struct Prover final : NoReturnCalleeProver {
    std::thread::id Owner = std::this_thread::get_id();
    mutable std::atomic<unsigned> Calls{0};
    mutable std::atomic<bool> Moved{false};
    bool mayNeverReturn(va_t) const override { return true; }
    bool neverReturns(va_t, unsigned) const override {
      ++Calls;
      if (std::this_thread::get_id() != Owner)
        Moved.store(true);
      return false;
    }
  } Counter;
  const auto Image = independentFloatingCalls(Arch::X64, BinaryFormat::ELF);
  const std::set<va_t> Entries{0x1000, 0x2000, 0x3000, 0x6000, 0x6020};
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Image));
  CFGBuilder Builder;
  Builder.setKnownFuncEntries(&Entries);
  Builder.setNoReturnCalleeProver(&Counter);
  Builder.setX87CallGraphCache(createX87CallGraphCache());
  Builder.setX87CallProofThreads(4);
  EXPECT_TRUE(
      Builder.build(Image, Dec, Image.Entry).hasCompleteInstructionLift());
  EXPECT_GT(Counter.Calls.load(), 0u);
  EXPECT_FALSE(Counter.Moved.load());
}

TEST(X87CallGraphCache,
     IdenticalConcurrentBuildsHaveOneLeaderAndPrivateBudgets) {
  using Access = detail::X87CallGraphCacheTestAccess;
  for (Arch Architecture : {Arch::X86, Arch::X64}) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    BinaryImage Image = x87CallLoopImage(Architecture, BinaryFormat::ELF);
    ReturningX87Prover Returning;
    CFGBuilder Cold, Limited;
    Cold.setNoReturnCalleeProver(&Returning);
    Limited.setNoReturnCalleeProver(&Returning);
    const auto Expected = Access::query(Cold, Image, 0x1100);
    const auto ExpectedLimited = Access::query(Limited, Image, 0x1100, 1);
    GatedX87Prover Prover;
    auto Cache = createX87CallGraphCache(&Prover);
    CFGBuilder Observer;
    Observer.setX87CallGraphCache(Cache);
    auto Run = [&](size_t Budget) {
      CFGBuilder Builder;
      Builder.setNoReturnCalleeProver(&Prover);
      Builder.setX87CallGraphCache(Cache);
      return Access::query(Builder, Image, 0x1100, Budget);
    };
    auto Leader = std::async(std::launch::async, Run,
                             size_t(limits::kMaxX87CallProofWork));
    EXPECT_TRUE(Prover.waitForCalls(1));
    auto Follower = std::async(std::launch::async, Run, size_t(1));
    EXPECT_TRUE(Access::waitForWaiters(Observer, 1));
    const auto Held = Access::flightStats(Observer);
    EXPECT_EQ(Held[0], 1u);
    EXPECT_EQ(Held[1], 1u);
    EXPECT_EQ(Held[3], 1u);
    EXPECT_EQ(Prover.calls(), 1u);
    Prover.release();
    EXPECT_EQ(Leader.get(), Expected);
    EXPECT_EQ(Follower.get(), ExpectedLimited);
    EXPECT_EQ(Prover.calls(), 1u);
    EXPECT_EQ(Access::flightStats(Observer)[3], 0u);
    EXPECT_EQ(Access::flightStats(Observer)[4], 0u);
  }
}

TEST(X87CallGraphCache, FailedLeaderWakesFollowersForIndependentRetry) {
  using Access = detail::X87CallGraphCacheTestAccess;
  BinaryImage Image = x87CallLoopImage(Arch::X86, BinaryFormat::ELF);
  ReturningX87Prover Returning;
  CFGBuilder Cold;
  Cold.setNoReturnCalleeProver(&Returning);
  const auto Expected = Access::query(Cold, Image, 0x1100);
  GatedX87Prover Prover(/*Throw=*/true);
  auto Cache = createX87CallGraphCache(&Prover);
  CFGBuilder Observer;
  Observer.setX87CallGraphCache(Cache);
  auto Run = [&] {
    CFGBuilder Builder;
    Builder.setNoReturnCalleeProver(&Prover);
    Builder.setX87CallGraphCache(Cache);
    return Access::query(Builder, Image, 0x1100);
  };
  auto Leader = std::async(std::launch::async, Run);
  EXPECT_TRUE(Prover.waitForCalls(1));
  auto Follower = std::async(std::launch::async, Run);
  EXPECT_TRUE(Access::waitForWaiters(Observer, 1));
  Prover.release();
  EXPECT_THROW((void)Leader.get(), std::runtime_error);
  EXPECT_EQ(Follower.get(), Expected);
  EXPECT_EQ(Prover.calls(), 2u);
  EXPECT_EQ(Run(), Expected);
  EXPECT_EQ(Prover.calls(),
            2u); // Failed leader did not poison later retention.
  EXPECT_EQ(Access::flightStats(Observer)[3], 0u);
  EXPECT_EQ(Access::flightStats(Observer)[4], 0u);
}

TEST(X87CallGraphCache, IncompleteLeaderReachesWaitersThroughTheirOwnProofs) {
  using Access = detail::X87CallGraphCacheTestAccess;
  BinaryImage Image = x87CallLoopImage(Arch::X86, BinaryFormat::ELF);
  Segment Leaf;
  Leaf.Name = ".leaf";
  Leaf.VA = 0x1200;
  Leaf.Size = 3;
  Leaf.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Leaf.Data = {0xd9, 0xe8, 0xc3};
  Image.Segments[0].Size = 0x106;
  Image.Segments[0].Data.resize(0x106);
  Image.Segments[0].Data.back() = 0x0f; // Truncated instruction after the call.
  Image.Segments.push_back(std::move(Leaf));
  GatedX87Prover Prover;
  auto Cache = createX87CallGraphCache(&Prover);
  CFGBuilder Observer;
  Observer.setX87CallGraphCache(Cache);
  auto Run = [&] {
    CFGBuilder Builder;
    Builder.setNoReturnCalleeProver(&Prover);
    Builder.setX87CallGraphCache(Cache);
    return Access::query(Builder, Image, 0x1100);
  };
  auto Leader = std::async(std::launch::async, Run);
  EXPECT_TRUE(Prover.waitForCalls(1));
  auto Follower = std::async(std::launch::async, Run);
  EXPECT_TRUE(Access::waitForWaiters(Observer, 1));
  Prover.release();
  const auto First = Leader.get();
  EXPECT_FALSE(First.first);
  EXPECT_EQ(First.second, limits::kMaxX87CallProofWork - 1);
  // The waiter takes the leader's refusal only after asking the prover the
  // leader's question itself, as every later request does.
  EXPECT_EQ(Follower.get(), First);
  EXPECT_EQ(Prover.calls(), 2u);
  EXPECT_EQ(Access::stats(Observer)[1], 1u);
  EXPECT_EQ(Run(), First);
  EXPECT_EQ(Prover.calls(), 3u);
  EXPECT_EQ(Access::flightStats(Observer)[3], 0u);
}

TEST(X87CallGraphCache, ConcurrentDifferentContextsNeverJoinTheSameFlight) {
  using Access = detail::X87CallGraphCacheTestAccess;
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    SCOPED_TRACE(Mutation);
    BinaryImage Image = x87CallLoopImage(Arch::X86, BinaryFormat::ELF);
    BinaryImage Other = x87CallLoopImage(Arch::X86, BinaryFormat::ELF, 5);
    GatedX87Prover Prover;
    auto Cache = createX87CallGraphCache(&Prover);
    const std::set<va_t> Empty, Different{0x12f0};
    CFGBuilder Observer;
    Observer.setX87CallGraphCache(Cache);
    auto Run = [&](bool Changed) {
      CFGBuilder Builder;
      Builder.setNoReturnCalleeProver(&Prover,
                                      Changed && Mutation == 0 ? 1 : 0);
      Builder.setKnownFuncEntries(Changed && Mutation == 1 ? &Different
                                                           : &Empty);
      Builder.setProtectedJumpTableRelocationSlots(
          Changed && Mutation == 2 ? &Different : &Empty);
      Builder.setX87CallGraphCache(Cache);
      return Access::query(Builder, Changed && Mutation == 3 ? Other : Image,
                           0x1100);
    };
    auto First = std::async(std::launch::async, Run, false);
    EXPECT_TRUE(Prover.waitForCalls(1));
    auto Second = std::async(std::launch::async, Run, true);
    // Both actual builders must reach the held callback. A context-blind join
    // cannot make progress to this point while the first leader is held.
    EXPECT_TRUE(Prover.waitForCalls(2));
    EXPECT_EQ(Access::flightStats(Observer)[1], 0u);
    EXPECT_EQ(Access::flightStats(Observer)[3], 2u);
    Prover.release();
    EXPECT_EQ(First.get().first, std::optional<int>(-1));
    EXPECT_EQ(Second.get().first, std::optional<int>(Mutation == 3 ? 0 : -1));
    EXPECT_EQ(Access::flightStats(Observer)[3], 0u);
  }
}

TEST(X87CallGraphCache,
     NestedAndUnregisteredAsyncProversNeverWaitOnThemselves) {
  using Access = detail::X87CallGraphCacheTestAccess;
  struct ReentrantProver final : NoReturnCalleeProver {
    const BinaryImage &Image;
    bool Async;
    std::shared_ptr<X87CallGraphCache> Cache;
    mutable std::atomic<bool> Entered{false};
    mutable std::optional<int> NestedResult;
    ReentrantProver(const BinaryImage &I, bool A) : Image(I), Async(A) {}
    bool mayNeverReturn(va_t) const override { return true; }
    bool neverReturns(va_t, unsigned) const override {
      if (!Entered.exchange(true)) {
        auto Run = [&] {
          CFGBuilder Nested;
          Nested.setNoReturnCalleeProver(this);
          Nested.setX87CallGraphCache(Cache);
          return Access::query(Nested, Image, 0x1100).first;
        };
        NestedResult =
            Async ? std::async(std::launch::async, Run).get() : Run();
      }
      return false;
    }
  };
  for (bool Async : {false, true}) {
    SCOPED_TRACE(Async);
    BinaryImage Image = x87CallLoopImage(Arch::X86, BinaryFormat::ELF);
    ReentrantProver Prover(Image, Async);
    // A synchronous nested request is guarded even for a registered callback.
    // An arbitrary callback that delegates then waits is not registered.
    auto Cache = createX87CallGraphCache(Async ? nullptr : &Prover);
    Prover.Cache = Cache;
    CFGBuilder Builder;
    Builder.setNoReturnCalleeProver(&Prover);
    Builder.setX87CallGraphCache(Cache);
    EXPECT_EQ(Access::query(Builder, Image, 0x1100).first,
              std::optional<int>(-1));
    EXPECT_EQ(Prover.NestedResult, std::optional<int>(-1));
    const auto Stats = Access::flightStats(Builder);
    EXPECT_GT(Stats[2], 0u);
    EXPECT_EQ(Stats[1], 0u);
    EXPECT_EQ(Stats[3], 0u);
    EXPECT_EQ(Stats[4], 0u);
    if (Async)
      EXPECT_EQ(Stats[0], 0u);
  }
}

TEST(X87CallGraphCache, NestedCrossCacheRequestsCannotFormWaitCycles) {
  using Access = detail::X87CallGraphCacheTestAccess;
  struct CrossCacheProver final : NoReturnCalleeProver {
    const BinaryImage &Image;
    std::barrier<> &BothLeaders;
    const CrossCacheProver *Other = nullptr;
    std::shared_ptr<X87CallGraphCache> Cache;
    mutable std::atomic<bool> Entered{false};
    mutable std::optional<int> NestedResult;
    CrossCacheProver(const BinaryImage &I, std::barrier<> &Gate)
        : Image(I), BothLeaders(Gate) {}
    bool mayNeverReturn(va_t) const override { return true; }
    bool neverReturns(va_t, unsigned) const override {
      if (!Entered.exchange(true)) {
        // Both outer flights exist before either callback requests the other
        // cache. Neither callback dispatches work to a different thread.
        BothLeaders.arrive_and_wait();
        CFGBuilder Nested;
        Nested.setNoReturnCalleeProver(Other);
        Nested.setX87CallGraphCache(Other->Cache);
        NestedResult = Access::query(Nested, Image, 0x1100).first;
      }
      return false;
    }
  };
  BinaryImage Image = x87CallLoopImage(Arch::X86, BinaryFormat::ELF);
  std::barrier BothLeaders(2);
  CrossCacheProver First(Image, BothLeaders), Second(Image, BothLeaders);
  First.Other = &Second;
  Second.Other = &First;
  First.Cache = createX87CallGraphCache(&First);
  Second.Cache = createX87CallGraphCache(&Second);
  auto Run = [&](const CrossCacheProver &Prover) {
    CFGBuilder Builder;
    Builder.setNoReturnCalleeProver(&Prover);
    Builder.setX87CallGraphCache(Prover.Cache);
    const auto Result = Access::query(Builder, Image, 0x1100);
    return std::make_pair(Result.first, Access::flightStats(Builder));
  };
  auto A = std::async(std::launch::async, Run, std::cref(First));
  auto B = std::async(std::launch::async, Run, std::cref(Second));
  size_t NestedMisses = 0;
  for (const auto &Result : {A.get(), B.get()}) {
    EXPECT_EQ(Result.first, std::optional<int>(-1));
    EXPECT_EQ(Result.second[1], 0u);
    NestedMisses += Result.second[2];
  }
  // One nested build may publish before its peer requests the other cache.
  EXPECT_GT(NestedMisses, 0u);
  EXPECT_EQ(First.NestedResult, std::optional<int>(-1));
  EXPECT_EQ(Second.NestedResult, std::optional<int>(-1));
}

TEST(X87CallGraphCache, UnregisteredNameResolverNeverReservesAFlight) {
  using Access = detail::X87CallGraphCacheTestAccess;
  BinaryImage Image = x87CallLoopImage(Arch::X86, BinaryFormat::ELF);
  Image.LoadOnlyFunctionEntries.insert(0x1100);
  Image.Symbols.clear();
  size_t Calls = 0;
  const libc::NoReturnTargetIndex Targets(Image, [&](va_t) {
    ++Calls;
    return std::optional<std::string>("ordinary_returning_function");
  });
  CFGBuilder Builder;
  Builder.setNoReturnTargetIndex(&Targets);
  Builder.setX87CallGraphCache(createX87CallGraphCache());
  EXPECT_EQ(Access::query(Builder, Image, 0x1100).first,
            std::optional<int>(-1));
  EXPECT_GT(Calls, 0u);
  const auto Stats = Access::flightStats(Builder);
  EXPECT_EQ(Stats[0], 0u);
  EXPECT_EQ(Stats[1], 0u);
  EXPECT_GT(Stats[2], 0u);
}

TEST(X87CallGraphCache, EvictionBoundsRetentionWithoutChangingResults) {
  using Access = detail::X87CallGraphCacheTestAccess;
  constexpr unsigned Count = 140;
  BinaryImage Image = x87CallLoopImage(Arch::X86, BinaryFormat::ELF);
  auto &Text = Image.Segments[0];
  Text.Size = Count * 16;
  Text.Data.assign(Text.Size, 0xcc);
  Image.Symbols.clear();
  Image.KnownCodeRanges.clear();
  std::set<va_t> Entries;
  for (unsigned I = 0; I != Count; ++I) {
    const va_t Address = Text.VA + I * 16;
    Text.Data[I * 16] = 0xd9;
    Text.Data[I * 16 + 1] = 0xe8;
    Text.Data[I * 16 + 2] = 0xc3;
    Entries.insert(Address);
    Image.Symbols.push_back(Symbol::makeFunc(Address, 3));
    Image.KnownCodeRanges.emplace_back(Address, Address + 3);
  }
  CFGBuilder Builder;
  Builder.setKnownFuncEntries(&Entries);
  Builder.setX87CallGraphCache(createX87CallGraphCache());
  for (va_t Address : Entries) {
    ASSERT_EQ(Access::query(Builder, Image, Address).first,
              std::optional<int>(-1));
    const auto Stats = Access::stats(Builder);
    EXPECT_LE(Stats[1], 128u);
    EXPECT_LE(Stats[2], 8u * 1024 * 1024);
  }
  const auto Before = Access::stats(Builder);
  EXPECT_EQ(Before[1], 128u);
  EXPECT_EQ(Access::query(Builder, Image, *Entries.begin()).first,
            std::optional<int>(-1));
  EXPECT_EQ(Access::stats(Builder)[0], Before[0]); // Oldest graph was evicted.
  EXPECT_EQ(Access::query(Builder, Image, *Entries.rbegin()).first,
            std::optional<int>(-1));
  EXPECT_EQ(Access::stats(Builder)[0], Before[0] + 1);
}

TEST_F(X86_32_X87FPU, NativeAndLiftedCallLoopsReturnTheIndependentSum) {
#if !defined(__linux__) || !defined(__x86_64__)
  GTEST_SKIP() << "requires a Linux x86-64 host with native i386 execution";
#else
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "requires Clang's x86 targets";
  for (Arch Architecture : {Arch::X86, Arch::X64})
    for (unsigned Shape : {0u, 1u, 14u, 15u})
      for (bool NoOpt : {false, true}) {
        const bool Extended = Shape == 1;
        SCOPED_TRACE(static_cast<int>(Architecture));
        SCOPED_TRACE(Shape);
        SCOPED_TRACE(NoOpt);
        const std::string Target = Architecture == Arch::X86
                                       ? "--target=i386-linux-gnu"
                                       : "--target=x86_64-linux-gnu";
        BinaryImage Image = x87CallLoopImage(Architecture, BinaryFormat::ELF,
                                             Extended ? 0 : Shape, Extended);
        llvm::LLVMContext Context;
        PipelineOptions Options;
        Options.EmitDumpOutput = false;
        Options.LiftMode = true;
        Options.NoOpt = NoOpt;
        Options.OnlyFunctionEntries = {0x1000, 0x1100, 0x1200};
        auto Result = Pipeline().run(Image, Context, Options);
        ASSERT_TRUE(Result.Success) << Result.Error;
        ASSERT_NE(Result.LlvmModule, nullptr);
        ASSERT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
        const auto LLVMFile = tmpFile("call-loop.ll");
        std::string IR;
        llvm::raw_string_ostream OS(IR);
        neverd::test::printHostCompilerFixture(*Result.LlvmModule, OS);
        std::ofstream(LLVMFile) << IR;
        Options.LiftMode = false;
        auto High = Pipeline().run(Image, Context, Options);
        ASSERT_TRUE(High.Success) << High.Error;
        ASSERT_EQ(High.HighFuncs.size(), 3u);
        const auto CFile = tmpFile("call-loop.c");
        std::string C = "#include <stdint.h>\n#include <stddef.h>\n";
        llvm::raw_string_ostream COS(C);
        CEmitterOptions Emit;
        Emit.TheArch = Architecture;
        Emit.Format = BinaryFormat::ELF;
        Emit.Image = &Image;
        Emit.EmitIncludes = false;
        ASSERT_TRUE(HighCEmitter().emit(High.HighFuncs, COS, Emit));
        std::ofstream(CFile) << C;
        // A separately emitted caller must declare the same return carrier
        // when its callee is linked from independently assembled machine code.
        const auto ExternalCFile = tmpFile("call-loop-external.c");
        std::string ExternalC = "#include <stdint.h>\n#include <stddef.h>\n";
        llvm::raw_string_ostream EOS(ExternalC);
        ASSERT_EQ(High.HighFuncs.front().Entry, 0x1000u);
        ASSERT_TRUE(HighCEmitter().emit({High.HighFuncs.front()}, EOS, Emit));
        std::ofstream(ExternalCFile) << ExternalC;
        const auto NativeFile = tmpFile("call-loop.s");
        std::ofstream Native(NativeFile);
        Native << ".text\n.globl x87_call_loop\nx87_call_loop:\n.byte ";
        const auto &Bytes = Image.Segments[0].Data;
        for (size_t I = 0; I < Bytes.size(); ++I)
          Native << (I ? "," : "") << unsigned(Bytes[I]);
        Native << "\n.section .note.GNU-stack,\"\",@progbits\n";
        Native.close();
        const auto NativeCalleesFile = tmpFile("call-loop-callees.s");
        std::ofstream NativeCallees(NativeCalleesFile);
        NativeCallees << ".text\n.globl x87_forwarder\nx87_forwarder:\n.byte ";
        for (size_t I = 0x100; I < Bytes.size(); ++I)
          NativeCallees << (I == 0x100 ? "" : ",") << unsigned(Bytes[I]);
        NativeCallees << "\n.section .note.GNU-stack,\"\",@progbits\n";
        NativeCallees.close();
        const auto HarnessFile = tmpFile("call-loop-start.s");
        std::ofstream Harness(HarnessFile);
        // 3 * (1 + 2^-60) has exponent 1 and low significand bits 12 in
        // binary80. A double-rounded return loses those bits completely.
        const uint32_t Expected = Extended ? 12 : UINT32_C(0x40080000);
        Harness << ".text\n.globl _start\n_start:\ncall x87_call_loop\ncmpl $"
                << Expected << ", %eax\nsetne %dl\n";
        if (Architecture == Arch::X86)
          Harness << "movzbl %dl,%ebx\nmovl $1,%eax\nint $0x80\n";
        else
          Harness << "movzbl %dl,%edi\nmovl $60,%eax\nsyscall\n";
        Harness << ".section .note.GNU-stack,\"\",@progbits\n";
        Harness.close();
        for (const std::string Opt : {"-O0", "-O2"})
          for (const auto &Source :
               {NativeFile, LLVMFile, CFile, ExternalCFile}) {
            SCOPED_TRACE(Opt);
            SCOPED_TRACE(Source.string());
            const auto Binary = tmpFile("call-loop-execution");
            std::vector<std::string> Args{Target,
                                          Opt,
                                          "-ffreestanding",
                                          "-fno-strict-aliasing",
                                          "-fuse-ld=lld",
                                          "-nostdlib",
                                          "-static",
                                          "-no-pie",
                                          Source.string(),
                                          HarnessFile.string(),
                                          "-o",
                                          Binary.string()};
            if (Source == ExternalCFile)
              Args.push_back(NativeCalleesFile.string());
            auto Build = exec(NEVERD_TEST_CLANG, Args);
            ASSERT_TRUE(Build.ok()) << Build.err << IR << C << ExternalC;
            const auto Run = exec(Binary.string(), {});
            ASSERT_TRUE(Run.ok()) << Run.err << IR << C << ExternalC;
          }
      }
#endif
}
} // namespace
