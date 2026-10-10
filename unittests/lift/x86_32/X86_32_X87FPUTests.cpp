#include "NeverDLiftFixture.h"

#include "neverd/Limits.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/ImportCallee.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <array>
#include <barrier>
#include <future>
#include <set>

namespace neverd::detail {
struct X87CallGraphCacheTestAccess {
  static std::array<size_t, 3> stats(const CFGBuilder &Builder) {
    return Builder.x87CallGraphCacheStatsForTesting();
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

TEST_F(X86_32_X87FPU, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(X86_32_X87FPU, NoUnreachable) {
    verifyLLVMIRNoUnreachable(testObj());
}

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
    verifyLowIRContains(testObj(), "test_fabs32", "FLOAT_ABS");
}

TEST_F(X86_32_X87FPU, FchsPreservesSemantics) {
    verifyLowIRContains(testObj(), "test_fchs32", "FLOAT_NEG");
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

TEST_F(X86_32_X87FPU, AllModesSucceed) {
    verifyAllModesSucceed(testObj());
}

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

bool hasX87CallDefinition(const LowFunc &Function) {
  for (const auto &B : Function.Blocks)
    for (const auto &Op : B.Ops)
      if ((Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
          Op.Output.isReg() && Op.Output.Size == x86reg::FPURegSize &&
          Op.Output.Offset >= x86reg::ST0 && Op.Output.Offset <= x86reg::ST7)
        return true;
  return false;
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
        const bool Supported = I != 1 && I != 5 && I != 6 && I != 8 && I != 9;
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
      }
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
        Lift.LlvmModule->print(OS, nullptr);
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

TEST(X87CallStack, ProvesCrossBlockResultsBeforeExpandingLoopStates) {
  for (Arch Architecture : {Arch::X86, Arch::X64})
    for (BinaryFormat Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (unsigned Mutation = 0; Mutation != 14; ++Mutation) {
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
        const bool Proven = Mutation == 0 || Mutation == 13;
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

TEST(X87CallGraphCache, IndependentBuildersRetainTheSameCallAndReturnIR) {
  for (Arch Architecture : {Arch::X86, Arch::X64}) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    BinaryImage Image = x87CallLoopImage(Architecture, BinaryFormat::ELF);
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

TEST(X87CallGraphCache, IncompleteInstructionGraphsAreNeverRetained) {
  using Access = detail::X87CallGraphCacheTestAccess;
  BinaryImage Image = x87CallLoopImage(Arch::X86, BinaryFormat::ELF);
  // The executable section ends halfway through FLD1. Ownership of the entry
  // does not turn a truncated instruction stream into a complete local graph.
  Image.Segments[0].Size = 0x201;
  Image.Segments[0].Data.resize(0x201);
  auto Cache = createX87CallGraphCache();
  for (unsigned I = 0; I != 2; ++I) {
    CFGBuilder Builder;
    Builder.setX87CallGraphCache(Cache);
    EXPECT_FALSE(Access::query(Builder, Image, 0x1200).first);
    EXPECT_EQ(Access::stats(Builder)[0], 0u);
    EXPECT_EQ(Access::stats(Builder)[1], 0u);
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
    for (bool Extended : {false, true})
      for (bool NoOpt : {false, true}) {
        SCOPED_TRACE(static_cast<int>(Architecture));
        SCOPED_TRACE(Extended);
        SCOPED_TRACE(NoOpt);
        const std::string Target = Architecture == Arch::X86
                                       ? "--target=i386-linux-gnu"
                                       : "--target=x86_64-linux-gnu";
        BinaryImage Image =
            x87CallLoopImage(Architecture, BinaryFormat::ELF, 0, Extended);
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
        Result.LlvmModule->print(OS, nullptr);
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
