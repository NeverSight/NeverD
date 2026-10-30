//===- InterpreterMachineStateTests.cpp - Recovery source ABI checks ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../lift/NeverDLiftFixture.h"
#include "../web/NativeFixture.h"

#include "neverd/analysis/InterpreterMachineState.h"
#include "neverd/analysis/arch/x86_64/BinaryInterpreterSpecialization.h"
#include "neverd/analysis/arch/x86_64/InterpreterMachineRegionSource.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/LowArchitecturalValue.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/LowToMedError.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/Loader.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Error.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>

namespace {
using namespace neverd;
using namespace neverd::analysis;

LowOp operation(NdOp Code, NdVar Output, std::initializer_list<NdVar> Inputs) {
  LowOp Op;
  Op.Opcode = Code;
  Op.Output = Output;
  for (const auto &Input : Inputs)
    Op.addInput(Input);
  return Op;
}

LowFunc function(std::initializer_list<std::vector<LowOp>> Instructions) {
  LowFunc Function;
  Function.Name = "generic_machine_source";
  Function.Entry = 0x1000;
  LowBlock Block;
  Block.Id = 0;
  Block.StartAddr = Function.Entry;
  va_t Address = Function.Entry;
  for (auto Ops : Instructions) {
    LowInstructionBoundary Boundary;
    Boundary.Address = Address++;
    Boundary.Size = 1;
    Boundary.FirstOp = Block.Ops.size();
    Boundary.OpCount = Ops.size();
    for (size_t Index = 0; Index != Ops.size(); ++Index) {
      Ops[Index].Addr = Boundary.Address;
      Ops[Index].Seq = static_cast<int>(Index);
      if (Ops[Index].Opcode == NdOp::RETURN) {
        Boundary.Control = LowInstructionControl::Return;
        Boundary.ControlFlags = LowInstructionControlFlag::Return;
      }
      Block.Ops.push_back(Ops[Index]);
    }
    Block.InstructionBoundaries.push_back(Boundary);
  }
  Block.EndAddr = Address;
  Function.Blocks.push_back(std::move(Block));
  return Function;
}

class InterpreterMachineSourceTest : public NeverDLiftTest {
protected:
  void
  roundTrip(const LowFunc &Function, llvm::StringRef Harness,
            llvm::StringRef Preamble = {},
            const BinaryImage *ConversionImage = nullptr,
            std::optional<InterpreterEntryAlignment> Alignment = std::nullopt,
            InterpreterMachineStateLayout Layout =
                InterpreterMachineStateLayout::InlineReturns,
            bool CPUQueryTape = false, bool VectorState = false,
            BinaryFormat SourceFormat = BinaryFormat::ELF) {
    auto Wrapped =
        VectorState    ? wrapInterpreterMachineQueryStateX64V2(
                             Function, SourceFormat,
                             InterpreterMachineStateProfile::UserX64NoFaultV1,
                             Alignment, Layout)
        : CPUQueryTape ? wrapInterpreterMachineQueryStateX64(
                             Function, SourceFormat,
                             InterpreterMachineStateProfile::UserX64NoFaultV1,
                             Alignment, Layout)
                       : wrapInterpreterMachineStateX64(
                             Function, SourceFormat,
                             InterpreterMachineStateProfile::UserX64NoFaultV1,
                             Alignment, Layout);
    ASSERT_TRUE(static_cast<bool>(Wrapped))
        << llvm::toString(Wrapped.takeError());
    emitAndRun(*Wrapped, Harness, Preamble, ConversionImage, CPUQueryTape,
               VectorState, SourceFormat);
  }

  void emitAndRun(const InterpreterMachineSource &Source,
                  llvm::StringRef Harness, llvm::StringRef Preamble = {},
                  const BinaryImage *ConversionImage = nullptr,
                  bool CPUQueryTape = false, bool VectorState = false,
                  BinaryFormat SourceFormat = BinaryFormat::ELF) {
    const auto *Wrapped = &Source;
    const auto &Function = Source.Function;
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(LLVM ? "LLVMC" : "HighC");
      LowToMedConverter Converter;
      Converter.setBinaryImage(ConversionImage);
      std::map<va_t, SourceFunctionTypeHint> Hints{
          {Function.Entry, Wrapped->SourceABI}};
      Converter.setSourceCallHintsEnabled(true);
      Converter.setSourceEntryTypeHints(&Hints);
      auto Med = Converter.convert(Wrapped->Function, Arch::X64);
      Med.SourceTypeHint = Wrapped->SourceABI;
      inferMedTypes(Med, Arch::X64);
      ASSERT_TRUE(verifyMedFunc(Med, "machine-source-test"));
      ASSERT_EQ(Med.Params.size(), 1u);
      EXPECT_EQ(Med.FrameSize, 0);
      std::string Source;
      llvm::raw_string_ostream OS(Source);
      CEmitterOptions Options;
      Options.TheArch = Arch::X64;
      Options.Format = SourceFormat;
      // Machine addresses may be unaligned and byte-alias one another. The
      // default listing casts do not promise those semantics to a C compiler.
      Options.UseUnalignedPointers = false;
      if (LLVM) {
        llvm::LLVMContext Context;
        MedLLVMEmitter Emitter;
        auto Module = Emitter.emit({Med}, Context);
        ASSERT_NE(Module, nullptr);
        ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
        ASSERT_TRUE(LLVMCEmitter().emit(*Module, OS, Options));
      } else {
        MedToHighConverter HighConverter;
        HighConverter.setBinaryImage(ConversionImage);
        auto High = HighConverter.convert(Med, Arch::X64);
        ASSERT_TRUE(HighCEmitter().emit({High}, OS, Options));
      }
      OS.flush();
      if (ConversionImage)
        for (const Symbol &Symbol : ConversionImage->Symbols)
          if (!Symbol.IsFunc)
            EXPECT_EQ(Source.find(Symbol.Name), std::string::npos) << Source;
      EXPECT_EQ(Source.find("__readeflags"), std::string::npos);
      EXPECT_EQ(Source.find("__writeeflags"), std::string::npos);
      if (CPUQueryTape || VectorState) {
        EXPECT_EQ(Source.find("cpuid"), std::string::npos);
        EXPECT_EQ(Source.find("__asm"), std::string::npos);
      }
      const auto SourceFile =
          tmpFile(LLVM ? "machine-llvm.c" : "machine-high.c");
      std::ofstream(SourceFile) << Preamble.str() << Source
                                << "\n#define MACHINE_ARG(p) ((uint8_t *)(p))\n"
                                << Harness.str();
      for (const char *Optimization : {"-O0", "-O2"}) {
        SCOPED_TRACE(Optimization);
        const auto Program = tmpFile(std::string("machine-source-test") +
                                     neverd::test::executableSuffix());
        const auto Built =
            exec(NEVERD_TEST_CLANG,
                 {"-std=c11", Optimization, "-Werror=return-type",
                  CPUQueryTape || VectorState ? "-fsanitize=address,undefined"
                                              : "-fsanitize=undefined",
                  "-fsanitize-trap=undefined", SourceFile.string(), "-o",
                  Program.string()});
        ASSERT_TRUE(Built.ok()) << Built.err << '\n' << Source;
        const auto Ran = exec(Program.string(), {});
        EXPECT_TRUE(Ran.ok()) << Ran.err << " exit " << Ran.exitCode << '\n'
                              << Source;
      }
    }
  }
};

std::vector<LowOp> cpuQueryInstruction() {
  Decoder Decoder;
  if (!Decoder.init(Arch::X64))
    return {};
  Decoder.setStrict(true);
  const uint8_t Bytes[]{0x0f, 0xa2};
  DecodedInsn Instruction{};
  if (Decoder.decodeOneForLift(Bytes, sizeof(Bytes), 0x1000, Instruction) !=
      sizeof(Bytes))
    return {};
  std::vector<LowOp> Ops;
  Decoder.liftToLow(Instruction, Ops);
  return Ops;
}

TEST_F(InterpreterMachineSourceTest, CPUQueryTapePreservesOrderedState) {
  const auto Query = cpuQueryInstruction();
  ASSERT_EQ(Query.size(), 9u);
  const auto Register = [](uint64_t Offset) { return NdVar::reg(Offset, 8); };
  auto F = function(
      {{operation(NdOp::STORE, {},
                  {Register(x86reg::RDI), NdVar::scalar(0x5a, 1)})},
       Query,
       {operation(NdOp::COPY, Register(x86reg::R8), {Register(x86reg::RAX)})},
       {operation(NdOp::COPY, Register(x86reg::RAX), {NdVar::scalar(7, 8)})},
       {operation(NdOp::COPY, Register(x86reg::RCX), {NdVar::scalar(3, 8)})},
       Query,
       {operation(NdOp::STORE, {},
                  {Register(x86reg::RDI), NdVar::scalar(0xb6, 1)})},
       {operation(NdOp::RETURN, {}, {})}});
  const char *Harness = R"C(
#include <stdint.h>
#include <string.h>
typedef struct { uint32_t leaf, subleaf, answer[4]; } query_t;
typedef struct {
  uint64_t gpr[16], flags, queries, count, cursor;
} state_t;
int main(void) {
  for (unsigned kind=0; kind<7; ++kind) {
    query_t tape[3] = {{7,3,{0x80000000u,11,12,13}},
                       {7,3,{21,0xffffffffu,23,24}},
                       {7,3,{0,0,0,0}}};
    unsigned char guest=0;
    state_t state={0}, expected;
    for (unsigned i=0;i<16;++i) state.gpr[i]=0x1234567800000000ull+i;
    state.gpr[0]=0x8765432100000007ull;
    state.gpr[1]=0xabcdef1200000003ull;
    state.gpr[7]=(uintptr_t)&guest;
    state.flags=0x246;
    state.queries=(uintptr_t)tape;
    state.count=2;
    if (kind==1) { state.count=0; state.queries=0; }
    if (kind==2) tape[0].leaf=8;
    if (kind==3) tape[0].subleaf=4;
    if (kind==4) state.count=1;
    if (kind==5) {
      tape[2]=tape[1]; tape[1]=tape[0]; tape[0].leaf=99;
      state.cursor=1; state.count=3;
    }
    if (kind==6) { state.cursor=UINT64_MAX; state.count=0; state.queries=0; }
    query_t original[3]; memcpy(original,tape,sizeof tape);
    expected=state;
    uint64_t status=0;
    if (kind==1 || kind==6) status=3;
    else if (kind==2 || kind==3) status=4;
    else {
      expected.gpr[8]=0x80000000u;
      expected.cursor++;
      if (kind==4) {
        status=3;
        expected.gpr[0]=7; expected.gpr[1]=3;
        expected.gpr[2]=13; expected.gpr[3]=11;
      } else {
        expected.gpr[0]=21; expected.gpr[1]=23;
        expected.gpr[2]=24; expected.gpr[3]=0xffffffffu;
        expected.cursor++;
      }
    }
    if (generic_machine_source(MACHINE_ARG(&state))!=status) return 1;
    if (memcmp(&state,&expected,sizeof state)) return 2;
    if (memcmp(tape,original,sizeof tape)) return 3;
    if (guest!=(status ? 0x5a : 0xb6)) return 4;
  }
  return 0;
}
)C";
  for (auto Layout : {InterpreterMachineStateLayout::InlineReturns,
                      InterpreterMachineStateLayout::SharedGPRExit})
    roundTrip(F, Harness, {}, nullptr, std::nullopt, Layout, true);
  auto OriginalABI = wrapInterpreterMachineStateX64(F);
  EXPECT_FALSE(bool(OriginalABI));
  if (!OriginalABI)
    llvm::consumeError(OriginalABI.takeError());
}

TEST(InterpreterMachineState, CPUQueryTapeRejectsIncompleteBundles) {
  const auto Query = cpuQueryInstruction();
  ASSERT_EQ(Query.size(), 9u);
  for (unsigned Kind = 0; Kind != 7; ++Kind) {
    auto Broken = Query;
    if (Kind == 0)
      Broken.pop_back();
    else if (Kind == 1)
      Broken[1].Output = NdVar::reg(x86reg::R8, 4);
    else if (Kind == 2)
      Broken[0].Inputs[1].Size = 8;
    else if (Kind == 3)
      Broken[1].Inputs[0] = NdVar::tmp(uint64_t{1} << 61, 4);
    else if (Kind == 4)
      Broken[0].Inputs[0].Offset = static_cast<uint64_t>(Intrinsic::Rdtsc);
    else if (Kind == 5)
      Broken[0].Inputs[1] = NdVar::reg(x86reg::XMM0, 4);
    else
      Broken[0].Inputs[1] = NdVar::tmp(uint64_t{1} << 61, 4);
    const auto F = function({Broken, {operation(NdOp::RETURN, {}, {})}});
    auto Result = wrapInterpreterMachineQueryStateX64(F);
    EXPECT_FALSE(bool(Result)) << Kind;
    if (!Result)
      llvm::consumeError(Result.takeError());
  }
}

TEST_F(InterpreterMachineSourceTest, CPUQueryTapeLoopsAndAlignment) {
  const auto Query = cpuQueryInstruction();
  ASSERT_EQ(Query.size(), 9u);
  LowFunc F;
  F.Name = "generic_machine_source";
  F.Entry = 0x1000;
  const auto Add = [&](int Id, va_t Address, std::vector<int> Preds,
                       std::vector<int> Succs,
                       std::initializer_list<std::vector<LowOp>> Instructions) {
    auto Part = function(Instructions);
    auto Block = std::move(Part.Blocks.front());
    const auto Delta = Address - Block.StartAddr;
    Block.Id = Id;
    Block.StartAddr += Delta;
    Block.EndAddr += Delta;
    Block.Preds = std::move(Preds);
    Block.Succs = std::move(Succs);
    for (auto &Op : Block.Ops)
      Op.Addr += Delta;
    for (auto &Boundary : Block.InstructionBoundaries) {
      Boundary.Address += Delta;
      const auto &Last = Block.Ops[Boundary.FirstOp + Boundary.OpCount - 1];
      if (Last.Opcode == NdOp::COND_BR) {
        Boundary.Control = LowInstructionControl::Branch;
        Boundary.ControlFlags = LowInstructionControlFlag::Branch |
                                LowInstructionControlFlag::Conditional;
        Boundary.Immediate = Last.Inputs[0].Offset;
      }
    }
    F.Blocks.push_back(std::move(Block));
  };
  const auto Reg = [](uint64_t Offset) { return NdVar::reg(Offset, 8); };
  const auto Constant = [](uint64_t Value) { return NdVar::scalar(Value, 8); };
  const auto Condition = NdVar::tmp(0, 1);
  Add(0, 0x1000, {}, {3, 1},
      {{operation(NdOp::COPY, Reg(x86reg::R11), {Constant(0)})},
       {operation(NdOp::INT_EQUAL, Condition, {Reg(x86reg::R10), Constant(0)}),
        operation(NdOp::COND_BR, {}, {NdVar::cst(0x1300, 8), Condition})}});
  Add(1, 0x1100, {0, 1}, {1, 2},
      {{operation(NdOp::COPY, Reg(x86reg::RAX), {Constant(7)})},
       {operation(NdOp::COPY, Reg(x86reg::RCX), {Constant(3)})},
       Query,
       {operation(NdOp::INT_ADD, Reg(x86reg::R11),
                  {Reg(x86reg::R11), Reg(x86reg::RAX)})},
       {operation(NdOp::INT_SUB, Reg(x86reg::R10),
                  {Reg(x86reg::R10), Constant(1)})},
       {operation(NdOp::INT_NOTEQUAL, Condition,
                  {Reg(x86reg::R10), Constant(0)}),
        operation(NdOp::COND_BR, {}, {NdVar::cst(0x1100, 8), Condition})}});
  Add(2, 0x1200, {1}, {}, {{operation(NdOp::RETURN, {}, {})}});
  Add(3, 0x1300, {0}, {}, {{operation(NdOp::RETURN, {}, {})}});
  const char *Harness = R"C(
#include <stdint.h>
#include <string.h>
typedef struct { uint32_t leaf, subleaf, answer[4]; } query_t;
typedef struct { uint64_t gpr[16], flags, queries, count, cursor; } state_t;
int main(void) {
  query_t tape[4];
  for (unsigned i=0;i<4;++i) {
    tape[i].leaf=7; tape[i].subleaf=3;
    for (unsigned j=0;j<4;++j) tape[i].answer[j]=0x80000000u+16*i+j;
  }
  for (unsigned iterations=0;iterations<6;++iterations) {
    state_t s={0}, expected;
    for (unsigned i=0;i<16;++i) s.gpr[i]=0xabcdef0100000000ull+i;
    s.gpr[4]=0x1008; s.gpr[10]=iterations;
    s.flags=0x202; s.queries=(uintptr_t)tape; s.count=4;
    expected=s; expected.gpr[11]=0;
    for (unsigned i=0;i<iterations && i<4;++i) {
      expected.gpr[0]=tape[i].answer[0];
      expected.gpr[3]=tape[i].answer[1];
      expected.gpr[1]=tape[i].answer[2];
      expected.gpr[2]=tape[i].answer[3];
      expected.gpr[11]+=tape[i].answer[0];
      expected.gpr[10]--; expected.cursor++;
    }
    uint64_t status=iterations>4 ? 3 : 0;
    if (status) { expected.gpr[0]=7; expected.gpr[1]=3; }
    if (generic_machine_source(MACHINE_ARG(&s))!=status) return 1;
    if (memcmp(&s,&expected,sizeof s)) return 2;
  }
  state_t bad={0}, before;
  bad.gpr[4]=0x1000; bad.gpr[10]=2; bad.gpr[11]=99; bad.flags=0x202;
  before=bad;
  if (generic_machine_source(MACHINE_ARG(&bad))!=2) return 3;
  if (memcmp(&bad,&before,sizeof bad)) return 4;
  return 0;
}

)C";
  for (auto Layout : {InterpreterMachineStateLayout::InlineReturns,
                      InterpreterMachineStateLayout::SharedGPRExit})
    roundTrip(F, Harness, {}, nullptr, InterpreterEntryAlignment{16, 8}, Layout,
              true);
}

TEST_F(InterpreterMachineSourceTest, CPUQueryTapeWrapsBinaryRecovery) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  Image.ExceptionMetadata.ParseStatus = ExceptionParseStatus::Complete;
  Segment Code;
  Code.VA = 0x1000;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  // MOV EAX,7; MOV ECX,3; STC; CPUID; RET. The original query is never executed
  // on the host; the source receives independent declared guest answers.
  Code.Data = {0xb8, 7, 0, 0, 0, 0xb9, 3, 0, 0, 0, 0xf9, 0x0f, 0xa2, 0xc3};
  Code.Size = Code.FileSz = Code.Data.size();
  Image.Segments.push_back(std::move(Code));
  SpecializationOptions Options;
  Options.ExplicitMachineState = true;
  Options.NormalNonfaultingExecution = true;
  Options.X64CetDisabled = true;
  Options.DiscoverControlState = true;
  Options.FrameBaseRegister = symbolic::SymRegisterRange{x86reg::RSP, 8};
  Options.RequireRestoredFrameAtReturn = true;
  auto Recovered = specializeBinaryInterpreter(Image, 0x1000, Options);
  ASSERT_TRUE(Recovered.complete()) << Recovered.Diagnostic;
  Recovered.Residual.Name = "generic_machine_source";
  roundTrip(Recovered.Residual, R"C(
#include <stdint.h>
#include <string.h>
int main(void) {
  uint32_t answers[6]={7,3,0xffffffffu,0x80000000u,0x12345678u,0x87654321u};
  uint64_t state[20], expected[20];
  for (unsigned i=0;i<16;++i) state[i]=0xabcdef0100000000ull+i;
  state[16]=0x246; state[17]=(uintptr_t)answers; state[18]=1; state[19]=0;
  memcpy(expected,state,sizeof state);
  expected[0]=answers[2]; expected[3]=answers[3];
  expected[1]=answers[4]; expected[2]=answers[5]; expected[19]=1;
  expected[16]=0x247;
  if (generic_machine_source(MACHINE_ARG(state))) return 1;
  return memcmp(state,expected,sizeof state)!=0;
}
)C",
            {}, nullptr, std::nullopt,
            InterpreterMachineStateLayout::InlineReturns, true);
}
#include "InterpreterMachineVectorSourceTests.inc"

LowFunc multipleReturns(bool Loop = false) {
  LowFunc F;
  F.Name = "generic_machine_source";
  F.Entry = 0x1000;
  const auto Add = [&](int Id, va_t Address, std::vector<int> Successors,
                       std::vector<LowOp> Ops) {
    auto Fragment = function({Ops});
    auto B = std::move(Fragment.Blocks.front());
    const va_t Delta = Address - B.StartAddr;
    B.Id = Id;
    B.StartAddr += Delta;
    B.EndAddr += Delta;
    B.Succs = std::move(Successors);
    for (auto &Op : B.Ops)
      Op.Addr += Delta;
    for (auto &Boundary : B.InstructionBoundaries) {
      Boundary.Address += Delta;
      const auto &Last = B.Ops[Boundary.FirstOp + Boundary.OpCount - 1];
      if (Last.Opcode == NdOp::COND_BR || Last.Opcode == NdOp::BRANCH) {
        Boundary.Control = LowInstructionControl::Branch;
        Boundary.ControlFlags = LowInstructionControlFlag::Branch;
        if (Last.Opcode == NdOp::COND_BR)
          Boundary.ControlFlags |= LowInstructionControlFlag::Conditional;
        Boundary.Immediate = Last.Inputs[0].Offset;
      }
    }
    F.Blocks.push_back(std::move(B));
  };
  const auto Selector = NdVar::reg(x86reg::RDX, 8);
  const auto Bit = NdVar::tmp(0x200, 8);
  const auto Condition = NdVar::tmp(0x208, 1);
  for (unsigned I = 0; I != 3; ++I) {
    const bool Entry = I == 0;
    const va_t Target = Entry ? 0x1200 : I == 1 ? 0x1400 : 0x1600;
    Add(I, 0x1000 + I * 0x100,
        Entry ? std::vector<int>{2, 1}
              : std::vector<int>{static_cast<int>(2 * I + 2),
                                 static_cast<int>(2 * I + 1)},
        {operation(NdOp::INT_AND, Bit,
                   {Selector, NdVar::scalar(Entry ? 2 : 1, 8)}),
         operation(NdOp::INT_NOTEQUAL, Condition, {Bit, NdVar::scalar(0, 8)}),
         operation(NdOp::COND_BR, {}, {NdVar::cst(Target, 8), Condition})});
  }
  for (unsigned I = 0; I != 4; ++I) {
    std::vector<LowOp> Ops;
    if (I == 3)
      Ops.push_back(
          operation(NdOp::INTRINSIC, {},
                    {NdVar::scalar(static_cast<uint64_t>(Intrinsic::Popf), 2),
                     NdVar::reg(x86reg::RCX, 8)}));
    const uint16_t Width = I & 1 ? 1 : 4;
    const uint64_t Register = x86reg::RAX + (I & 1);
    const std::vector<LowOp> Tail{
        operation(NdOp::COPY, NdVar::reg(Register, Width),
                  {NdVar::scalar(0x30 + I, Width)}),
        operation(NdOp::INT_ADD, NdVar::reg(x86reg::R8, 8),
                  {NdVar::reg(x86reg::R8, 8), NdVar::scalar(7 * I + 1, 8)}),
        operation(NdOp::COPY, NdVar::reg(x86reg::CF, 1),
                  {NdVar::scalar(I & 1, 1)}),
        operation(NdOp::COPY, NdVar::reg(x86reg::ZF, 1),
                  {NdVar::scalar(I >> 1, 1)}),
        operation(NdOp::STORE, {},
                  {NdVar::reg(x86reg::RBX, 8), NdVar::reg(x86reg::R8, 8)}),
        operation(NdOp::RETURN, {}, {NdVar::reg(x86reg::RAX, 8)})};
    Ops.insert(Ops.end(), Tail.begin(), Tail.end());
    Add(I + 3, 0x1300 + I * 0x100, {}, std::move(Ops));
  }
  if (Loop) {
    Add(7, 0x800, {8},
        {operation(NdOp::COPY, NdVar::reg(x86reg::R10, 8),
                   {NdVar::scalar(0, 8)}),
         operation(NdOp::BRANCH, {}, {NdVar::cst(0x900, 8)})});
    Add(8, 0x900, {8, 0},
        {operation(NdOp::INT_ADD, NdVar::reg(x86reg::R10, 8),
                   {NdVar::reg(x86reg::R10, 8), NdVar::scalar(1, 8)}),
         operation(NdOp::INT_AND, Bit,
                   {NdVar::reg(x86reg::R11, 8), NdVar::scalar(7, 8)}),
         operation(NdOp::INT_LESS, Condition,
                   {NdVar::reg(x86reg::R10, 8), Bit}),
         operation(NdOp::COND_BR, {}, {NdVar::cst(0x900, 8), Condition})});
    F.Entry = 0x800;
  }
  for (const auto &B : F.Blocks)
    for (int Next : B.Succs)
      F.Blocks[Next].Preds.push_back(B.Id);
  if (Loop)
    std::rotate(F.Blocks.begin(), F.Blocks.end() - 2, F.Blocks.end());
  return F;
}

TEST(InterpreterMachineStateTest, SourceWrapperRefusesUnloweredCalls) {
  for (auto Format :
       {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
    for (auto Layout : {InterpreterMachineStateLayout::InlineReturns,
                        InterpreterMachineStateLayout::SharedGPRExit})
      for (auto Opcode : {NdOp::CALL, NdOp::INDIR_CALL, NdOp::INDIR_BR}) {
        SCOPED_TRACE(static_cast<unsigned>(Format));
        SCOPED_TRACE(static_cast<unsigned>(Layout));
        SCOPED_TRACE(static_cast<unsigned>(Opcode));
        const bool Direct = Opcode == NdOp::CALL;
        auto F =
            function({{operation(Opcode, {},
                                 {Direct ? NdVar::codeAddress(0x2000, 8, 0x2000)
                                         : NdVar::reg(x86reg::RAX, 8)})}});
        auto &Boundary = F.Blocks.front().InstructionBoundaries.front();
        Boundary.Control = Opcode == NdOp::INDIR_BR
                               ? LowInstructionControl::Branch
                               : LowInstructionControl::Call;
        Boundary.ControlFlags = Opcode == NdOp::INDIR_BR
                                    ? LowInstructionControlFlag::Branch
                                    : LowInstructionControlFlag::Call;
        if (Direct)
          Boundary.Immediate = 0x2000;
        else
          Boundary.ControlFlags |= LowInstructionControlFlag::Indirect;
        auto Wrapped = wrapInterpreterMachineStateX64(
            F, Format, InterpreterMachineStateProfile::UserX64NoFaultV1,
            std::nullopt, Layout);
        ASSERT_FALSE(static_cast<bool>(Wrapped));
        EXPECT_NE(llvm::toString(Wrapped.takeError())
                      .find("requires resolved call-free control flow"),
                  std::string::npos);
      }
}

TEST(InterpreterMachineStateTest, SharedReturnStateWritesStayBounded) {
  const auto F = multipleReturns();
  for (auto Format :
       {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
    auto Wrapped = wrapInterpreterMachineStateX64(
        F, Format, InterpreterMachineStateProfile::UserX64NoFaultV1,
        std::nullopt, InterpreterMachineStateLayout::SharedGPRExit);
    ASSERT_TRUE(static_cast<bool>(Wrapped))
        << llvm::toString(Wrapped.takeError());
    unsigned Stores = 0, Returns = 0;
    for (const auto &B : Wrapped->Function.Blocks)
      for (const auto &Op : B.Ops) {
        Stores += Op.Opcode == NdOp::STORE;
        Returns += Op.Opcode == NdOp::RETURN;
      }
    // Four guest writes and four packed flag writes stay on their original
    // paths. The sixteen GPR words have one common commit.
    EXPECT_EQ(Stores, 4U + 4U + 16U);
    EXPECT_EQ(Returns, 1U);
  }
  auto Model = modelInterpreterMachineStateX64(
      F, InterpreterMachineStateProfile::UserX64NoFaultV1, 65536, std::nullopt,
      InterpreterMachineStateLayout::SharedGPRExit);
  ASSERT_TRUE(static_cast<bool>(Model)) << llvm::toString(Model.takeError());
  unsigned Returns = 0;
  for (const auto &B : Model->Function.Blocks)
    for (const auto &Op : B.Ops)
      Returns += Op.Opcode == NdOp::RETURN;
  EXPECT_EQ(Returns, 1U);
}

TEST(InterpreterMachineStateTest, SharedReturnUsesFreshIdentityAndMetadata) {
  auto F = multipleReturns();
  const auto NewID = [](int Id) {
    return Id ? Id * 17 : std::numeric_limits<int>::max();
  };
  for (auto &B : F.Blocks) {
    B.Id = NewID(B.Id);
    for (auto *Edges : {&B.Preds, &B.Succs})
      for (auto &Id : *Edges)
        Id = NewID(Id);
    if (B.Ops.back().Opcode == NdOp::RETURN)
      B.InstructionBoundaries.back().Immediate = 8;
  }
  auto Wrapped = wrapInterpreterMachineStateX64(
      F, BinaryFormat::ELF, InterpreterMachineStateProfile::UserX64NoFaultV1,
      std::nullopt, InterpreterMachineStateLayout::SharedGPRExit);
  ASSERT_TRUE(static_cast<bool>(Wrapped))
      << llvm::toString(Wrapped.takeError());
  const auto &Exit = Wrapped->Function.Blocks.back();
  EXPECT_EQ(Exit.Id, 0);
  EXPECT_EQ(Exit.Preds.size(), 4U);
  EXPECT_EQ(Exit.InstructionBoundaries.back().Control,
            LowInstructionControl::Return);
  EXPECT_FALSE(Exit.InstructionBoundaries.back().Immediate);
  for (size_t I = 3; I != 7; ++I) {
    const auto &B = Wrapped->Function.Blocks[I];
    EXPECT_EQ(B.Succs, std::vector<int>{Exit.Id});
    EXPECT_EQ(B.Ops.back().Opcode, NdOp::BRANCH);
    EXPECT_EQ(B.Ops.back().Inputs[0].Offset, Exit.StartAddr);
    EXPECT_EQ(B.InstructionBoundaries.back().Immediate, Exit.StartAddr);
    EXPECT_EQ(B.InstructionBoundaries.back().ControlFlags,
              LowInstructionControlFlag::Branch);
  }
}

TEST(InterpreterMachineStateTest, DefaultReturnLayoutRemainsInline) {
  const auto F = multipleReturns();
  auto Default = wrapInterpreterMachineStateX64(F);
  auto Inline = wrapInterpreterMachineStateX64(
      F, BinaryFormat::ELF, InterpreterMachineStateProfile::UserX64NoFaultV1,
      std::nullopt, InterpreterMachineStateLayout::InlineReturns);
  ASSERT_TRUE(bool(Default)) << llvm::toString(Default.takeError());
  ASSERT_TRUE(bool(Inline)) << llvm::toString(Inline.takeError());
  ASSERT_EQ(Default->Function.Blocks.size(), F.Blocks.size());
  ASSERT_EQ(Inline->Function.Blocks.size(), F.Blocks.size());
  unsigned Stores = 0, Returns = 0;
  for (size_t I = 0; I != F.Blocks.size(); ++I) {
    const auto &B = Default->Function.Blocks[I];
    EXPECT_EQ(lowUndefinedOperationDigest(B.Ops),
              lowUndefinedOperationDigest(Inline->Function.Blocks[I].Ops));
    for (const auto &Op : B.Ops) {
      Stores += Op.Opcode == NdOp::STORE;
      Returns += Op.Opcode == NdOp::RETURN;
    }
  }
  EXPECT_EQ(Stores, 4U + 4U * 17U);
  EXPECT_EQ(Returns, 4U);
}

TEST(InterpreterMachineStateTest, RejectsUnsupportedReturnLayouts) {
  const auto F = multipleReturns();
  const auto InvalidLayout = static_cast<InterpreterMachineStateLayout>(255);
  auto Wrapped = wrapInterpreterMachineStateX64(
      F, BinaryFormat::ELF, InterpreterMachineStateProfile::UserX64NoFaultV1,
      std::nullopt, InvalidLayout);
  EXPECT_FALSE(bool(Wrapped));
  llvm::consumeError(Wrapped.takeError());
  auto Model = modelInterpreterMachineStateX64(
      F, InterpreterMachineStateProfile::UserX64NoFaultV1, 65536, std::nullopt,
      InvalidLayout);
  EXPECT_FALSE(bool(Model));
  llvm::consumeError(Model.takeError());
}

TEST(InterpreterMachineStateTest, SharedReturnReservesAlignmentLayoutSpace) {
  for (bool Align : {false, true}) {
    auto F = multipleReturns();
    const va_t End = InvalidVA - (Align ? 4 : 2);
    const va_t Delta = End - F.Blocks.back().EndAddr;
    F.Entry += Delta;
    for (auto &B : F.Blocks) {
      B.StartAddr += Delta;
      B.EndAddr += Delta;
      for (auto &Boundary : B.InstructionBoundaries) {
        Boundary.Address += Delta;
        if (Boundary.Control == LowInstructionControl::Branch)
          *Boundary.Immediate += Delta;
      }
      for (auto &Op : B.Ops) {
        Op.Addr += Delta;
        if (Op.Opcode == NdOp::COND_BR)
          Op.Inputs[0].Offset += Delta;
      }
    }
    auto Wrapped = wrapInterpreterMachineStateX64(
        F, BinaryFormat::ELF, InterpreterMachineStateProfile::UserX64NoFaultV1,
        Align ? std::optional{InterpreterEntryAlignment{16, 8}} : std::nullopt,
        InterpreterMachineStateLayout::SharedGPRExit);
    ASSERT_TRUE(static_cast<bool>(Wrapped))
        << llvm::toString(Wrapped.takeError());
    unsigned Returns = 0;
    for (const auto &B : Wrapped->Function.Blocks)
      for (const auto &Op : B.Ops)
        Returns += Op.Opcode == NdOp::RETURN;
    EXPECT_EQ(Returns, 4U + Align);
  }
}

TEST(InterpreterMachineStateTest, SharedReturnDoesNotBindDanglingReferences) {
  for (unsigned Case = 0; Case != 4; ++Case) {
    auto F = multipleReturns();
    if (Case == 0) {
      const va_t Missing = F.Blocks.back().EndAddr + 1;
      F.Blocks.front().Ops.back().Inputs[0].Offset = Missing;
      F.Blocks.front().InstructionBoundaries.back().Immediate = Missing;
    } else if (Case == 1) {
      F.Blocks.front().Succs.push_back(7);
    } else if (Case == 2) {
      F.Blocks.back().Succs.push_back(2);
    } else {
      auto &B = F.Blocks.back();
      LowInstructionBoundary Boundary;
      Boundary.Address = B.EndAddr++;
      Boundary.Size = 1;
      Boundary.FirstOp = B.Ops.size();
      Boundary.OpCount = 1;
      auto Op = operation(NdOp::COPY, NdVar::reg(x86reg::RAX, 8),
                          {NdVar::scalar(9, 8)});
      Op.Addr = Boundary.Address;
      B.Ops.push_back(Op);
      B.InstructionBoundaries.push_back(Boundary);
    }
    auto Wrapped = wrapInterpreterMachineStateX64(
        F, BinaryFormat::ELF, InterpreterMachineStateProfile::UserX64NoFaultV1,
        std::nullopt, InterpreterMachineStateLayout::SharedGPRExit);
    ASSERT_TRUE(static_cast<bool>(Wrapped))
        << llvm::toString(Wrapped.takeError());
    // Compaction must not bind missing targets/IDs or erase a malformed
    // return edge. Their original validation belongs to the CFG consumer.
    EXPECT_EQ(Wrapped->Function.Blocks.size(), F.Blocks.size());
    EXPECT_EQ(Wrapped->Function.Blocks.front().Succs, F.Blocks.front().Succs);
    EXPECT_EQ(Wrapped->Function.Blocks.back().Succs, F.Blocks.back().Succs);
    EXPECT_EQ(Wrapped->Function.Blocks.front().Ops.back().Inputs[0].Offset,
              F.Blocks.front().Ops.back().Inputs[0].Offset);
  }
}

TEST(InterpreterMachineStateTest, SharedReturnModelHonorsBothWorkBounds) {
  const auto F = multipleReturns();
  auto Model = modelInterpreterMachineStateX64(
      F, InterpreterMachineStateProfile::UserX64NoFaultV1, 65536, std::nullopt,
      InterpreterMachineStateLayout::SharedGPRExit);
  ASSERT_TRUE(static_cast<bool>(Model)) << llvm::toString(Model.takeError());
  uint64_t Input = F.Blocks.size() + F.ModuleAnalysisRoots.size() +
                   F.OrdinaryModuleAnalysisRoots.size();
  for (const auto &B : F.Blocks)
    Input += B.Ops.size() + B.InstructionBoundaries.size() + B.Preds.size() +
             B.Succs.size() + B.ExceptionalPreds.size() +
             B.ExceptionalSuccs.size();
  uint64_t Output = 0;
  for (const auto &B : Model->Function.Blocks)
    Output += B.Ops.size();
  for (uint64_t Limit : {Input - 1, Output - 1}) {
    auto Short = modelInterpreterMachineStateX64(
        F, InterpreterMachineStateProfile::UserX64NoFaultV1, Limit,
        std::nullopt, InterpreterMachineStateLayout::SharedGPRExit);
    EXPECT_FALSE(static_cast<bool>(Short));
    llvm::consumeError(Short.takeError());
  }
  auto Exact = modelInterpreterMachineStateX64(
      F, InterpreterMachineStateProfile::UserX64NoFaultV1,
      std::max(Input, Output), std::nullopt,
      InterpreterMachineStateLayout::SharedGPRExit);
  ASSERT_TRUE(static_cast<bool>(Exact)) << llvm::toString(Exact.takeError());
}

TEST_F(InterpreterMachineSourceTest,
       SharedReturnPreservesEveryPathAndAlignmentRejection) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "machine source execution requires clang";
  const auto Harness = R"C(
#include <string.h>
int main(void) {
  for (unsigned seed = 0; seed != 32; ++seed)
    for (unsigned lane = 0; lane != 4; ++lane)
      for (unsigned mode = 0; mode != 3; ++mode) {
        uint64_t state[17], before[17], expected[17];
        uint64_t memory[3] = {0x12345678, 0, 0xabcdef01};
        for (unsigned i = 0; i != 16; ++i)
          state[i] = UINT64_C(0xfedcba9876543210) ^ (91 * seed + 17 * i);
        state[1] = seed & 1 ? 0x204102 : 2;
        state[2] = (UINT64_C(0x1234000000000000) | (seed << 2)) | lane;
        state[3] = (uint64_t)(uintptr_t)&memory[1];
        state[4] = 0x8008 + (mode == 2);
        state[16] = mode == 1 ? 0 : (seed & 1) ? 0xcd7 : 2;
        memcpy(before, state, sizeof state);
        memcpy(expected, state, sizeof state);
        const uint64_t status = generic_machine_source(MACHINE_ARG(state));
        if (mode == 2) {
          if (status != 2 || memcmp(state, before, sizeof state) || memory[1])
            return 1;
        } else {
          expected[0] = lane & 1
              ? (before[0] & ~UINT64_C(0xff00)) | ((0x30 + lane) << 8)
              : 0x30 + lane;
          expected[8] += 7 * lane + 1;
          if (LOOP_PREFIX)
            expected[10] = (before[11] & 7) ? before[11] & 7 : 1;
          expected[16] = (before[16] & ~UINT64_C(0x41)) |
                         (lane & 1) | ((lane >> 1) << 6);
          if (lane == 3)
            expected[16] = (expected[16] & ~UINT64_C(0x204000)) |
                           (before[1] & UINT64_C(0x204000));
          const unsigned failed = mode == 1 || (lane == 3 && (seed & 1));
          if (status != failed || memcmp(state, expected, sizeof state) ||
              memory[1] != expected[8]) return 2;
        }
        if (memory[0] != 0x12345678 || memory[2] != 0xabcdef01) return 3;
      }
  return 0;
}
)C";
  for (auto Layout : {InterpreterMachineStateLayout::InlineReturns,
                      InterpreterMachineStateLayout::SharedGPRExit})
    for (bool Loop : {false, true})
      roundTrip(multipleReturns(Loop), Harness,
                Loop ? "#define LOOP_PREFIX 1\n" : "#define LOOP_PREFIX 0\n",
                nullptr, InterpreterEntryAlignment{16, 8}, Layout);
}

TEST_F(InterpreterMachineSourceTest, AlignmentRejectsBeforeGuestOrStateWrites) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "machine source execution requires clang";
  const auto RSP = NdVar::reg(x86reg::RSP, 8);
  const auto RAX = NdVar::reg(x86reg::RAX, 8);
  const auto F =
      function({{operation(NdOp::STORE, {}, {RSP, NdVar::scalar(90, 1)})},
                {operation(NdOp::COPY, RAX, {NdVar::scalar(37, 8)})},
                {operation(NdOp::RETURN, {}, {RAX})}});
  roundTrip(F, R"C(
#include <stdint.h>
#include <string.h>
int main(void) {
  _Alignas(32) uint8_t memory[32] = {0};
  uint64_t state[17], before[17];
  for (unsigned residue = 0; residue < 16; ++residue) {
    for (unsigned i = 0; i < 17; ++i) state[i] = i * 137 + 59;
    state[16] = residue == 3 ? 2 : 0; // Alignment rejection precedes bad flags.
    /* Every rejected root is inaccessible, even for a one-byte store. */
    state[4] = residue == 3 ? (uint64_t)(uintptr_t)(memory + 3) : residue;
    memcpy(before, state, sizeof state);
    uint64_t result = generic_machine_source(MACHINE_ARG(state));
    if (residue != 3) {
      if (result != 2 || memcmp(before, state, sizeof state)) return 1;
    } else {
      before[0] = 37;
      if (result || memcmp(before, state, sizeof state)) return 2;
      if (memory[3] != 90) return 3;
    }
    for (unsigned i = 0; i < 32; ++i)
      if (i != 3 && memory[i]) return 4;
  }
  return 0;
}
)C",
            {}, nullptr, InterpreterEntryAlignment{16, 3});
}

TEST(InterpreterMachineStateTest, V1RefusesRetainedSegmentedMemory) {
  for (auto Space :
       {NdMemoryAddressSpace::X86FS, NdMemoryAddressSpace::X86GS}) {
    for (bool Write : {false, true}) {
      auto Access =
          Write ? operation(NdOp::STORE, {},
                            {NdVar::scalar(16, 8), NdVar::reg(x86reg::RAX, 8)})
                : operation(NdOp::LOAD, NdVar::reg(x86reg::RAX, 8),
                            {NdVar::scalar(16, 8)});
      Access.MemoryAddressSpace = Space;
      const auto F = function({{Access}, {operation(NdOp::RETURN, {}, {})}});
      auto Wrapped = wrapInterpreterMachineStateX64(F);
      ASSERT_FALSE(static_cast<bool>(Wrapped));
      EXPECT_NE(llvm::toString(Wrapped.takeError()).find("special memory"),
                std::string::npos);
    }
  }
}

TEST(InterpreterMachineStateTest, OrderedAtomicsStillRefusesSourceWrapping) {
  for (auto Opcode :
       {NdOp::ATOMIC_CMPXCHG, NdOp::ATOMIC_ADD, NdOp::ATOMIC_XCHG}) {
    const auto Address = NdVar::reg(x86reg::RCX, 8);
    const auto Expected = NdVar::reg(x86reg::RDX, 8);
    const auto Desired = NdVar::reg(x86reg::R8, 8);
    const auto Old = NdVar::reg(x86reg::RAX, 8);
    auto Control =
        function({{operation(NdOp::COPY, Address, {NdVar::scalar(0x4000, 8)}),
                   operation(NdOp::COPY, Expected, {NdVar::scalar(7, 8)}),
                   operation(NdOp::COPY, Desired, {NdVar::scalar(11, 8)}),
                   operation(NdOp::STORE, {}, {Address, Expected})},
                  {operation(NdOp::LOAD, Old, {Address})},
                  {operation(NdOp::RETURN, {}, {Old})}});
    auto Ordered = Control;
    auto &Access = Ordered.Blocks.front().Ops[4];
    Access.Opcode = Opcode;
    Access.addInput(Expected);
    if (Opcode == NdOp::ATOMIC_CMPXCHG)
      Access.addInput(Desired);
    Access.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
    auto Boundaries = validateLowInstructionBoundaries(
        Ordered, LowInstructionBoundaryRequirement::Required);
    ASSERT_FALSE(bool(Boundaries)) << llvm::toString(std::move(Boundaries));

    // The ordinary control establishes valid operands, initialization and CFG.
    // A raw atomic operation alone must not expand this source ABI contract.
    for (auto Layout : {InterpreterMachineStateLayout::InlineReturns,
                        InterpreterMachineStateLayout::SharedGPRExit}) {
      auto WrappedControl = wrapInterpreterMachineStateX64(
          Control, BinaryFormat::ELF,
          InterpreterMachineStateProfile::UserX64NoFaultV1, std::nullopt,
          Layout);
      ASSERT_TRUE(bool(WrappedControl))
          << llvm::toString(WrappedControl.takeError());
      auto Wrapped = wrapInterpreterMachineStateX64(
          Ordered, BinaryFormat::ELF,
          InterpreterMachineStateProfile::UserX64NoFaultV1, std::nullopt,
          Layout);
      ASSERT_FALSE(bool(Wrapped));
      EXPECT_NE(llvm::toString(Wrapped.takeError())
                    .find("cannot wrap ordered or special memory"),
                std::string::npos);
    }
  }
}

TEST(InterpreterMachineStateTest, V1RefusesTaskPriorityIntrinsics) {
  for (auto Op :
       {operation(
            NdOp::INTRINSIC, NdVar::reg(x86reg::RAX, 8),
            {NdVar::scalar(static_cast<uint64_t>(Intrinsic::ReadCr8), 2)}),
        operation(NdOp::INTRINSIC, {},
                  {NdVar::scalar(static_cast<uint64_t>(Intrinsic::WriteCr8), 2),
                   NdVar::scalar(0, 8)})}) {
    auto Wrapped = wrapInterpreterMachineStateX64(
        function({{Op}, {operation(NdOp::RETURN, {}, {})}}));
    ASSERT_FALSE(static_cast<bool>(Wrapped));
    llvm::consumeError(Wrapped.takeError());
  }
}

TEST(InterpreterMachineStateTest, V1RefusesMXCSRIntrinsics) {
  for (auto Op :
       {operation(NdOp::INTRINSIC, {},
                  {NdVar::scalar(uint64_t(Intrinsic::Ldmxcsr), 2),
                   NdVar::reg(x86reg::RBX, 8)}),
        operation(NdOp::INTRINSIC, NdVar::reg(x86reg::RAX, 4),
                  {NdVar::scalar(uint64_t(Intrinsic::X86ReadMXCSR), 2)}),
        operation(NdOp::INTRINSIC, {},
                  {NdVar::scalar(uint64_t(Intrinsic::X86WriteMXCSR), 2),
                   NdVar::scalar(0x1f80, 4)})}) {
    auto Wrapped = wrapInterpreterMachineStateX64(
        function({{Op}, {operation(NdOp::RETURN, {}, {})}}));
    ASSERT_FALSE(bool(Wrapped));
    llvm::consumeError(Wrapped.takeError());
  }
}

TEST(InterpreterMachineStateTest, V1RefusesWideTransport) {
  for (bool Temporary : {false, true}) {
    const auto Output =
        Temporary ? NdVar::tmp(64, 16) : NdVar::reg(x86reg::XMM0, 16);
    const auto F =
        function({{operation(NdOp::COPY, Output, {NdVar::scalar(0, 16)})},
                  {operation(NdOp::RETURN, {}, {})}});
    for (auto Layout : {InterpreterMachineStateLayout::InlineReturns,
                        InterpreterMachineStateLayout::SharedGPRExit}) {
      auto Wrapped = wrapInterpreterMachineStateX64(
          F, BinaryFormat::ELF,
          InterpreterMachineStateProfile::UserX64NoFaultV1, std::nullopt,
          Layout);
      ASSERT_FALSE(static_cast<bool>(Wrapped));
      EXPECT_NE(llvm::toString(Wrapped.takeError())
                    .find(Temporary ? "invalid temporary range"
                                    : "unsupported register"),
                std::string::npos);
    }
  }
}

TEST(InterpreterMachineStateTest, PipelineRefusesUnmappedSegmentEnvironment) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  Image.ExceptionMetadata.ParseStatus = ExceptionParseStatus::Complete;
  Segment Code;
  Code.VA = 0x1000;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Code.Data = {0xb8, 7, 0, 0, 0, 0xc3};
  Code.Size = Code.FileSz = Code.Data.size();
  Image.Segments.push_back(std::move(Code));
  PipelineOptions Options;
  Options.OnlyFunctionEntries.insert(0x1000);
  Options.InterpreterSpecialization.emplace();
  Options.InterpreterSpecialization->ExplicitMachineState = true;
  Options.InterpreterSpecialization->NormalNonfaultingExecution = true;
  llvm::LLVMContext Context;
  const char *Diagnostics[] = {
      "task-priority source interface", "FS/GS base source interface",
      "FS/GS base source interface", "MXCSR source interface",
      "legacy GS source interface"};
  for (unsigned Kind = 0; Kind != 5; ++Kind) {
    SCOPED_TRACE(Kind);
    Options.InterpreterSpecialization->X64SegmentedReads = Kind == 1;
    Options.InterpreterSpecialization->X64SegmentedWrites = Kind == 2;
    Options.InterpreterSpecialization->X64CR8State = Kind == 0;
    Options.InterpreterSpecialization->X64LegacyGSState = Kind == 4;
    Options.InterpreterSpecialization->X64MXCSRMask =
        Kind == 3 ? std::optional<uint32_t>(0xffbf) : std::nullopt;
    Options.InterpreterSpecialization->X64FlagsProfile =
        InterpreterMachineStateProfile::KernelX64NoFaultV1;
    Pipeline P;
    const auto Result = P.run(Image, Context, Options);
    EXPECT_FALSE(Result.Success);
    EXPECT_NE(Result.Error.find(Diagnostics[Kind]), std::string::npos);
    EXPECT_FALSE(Result.LlvmModule);
  }
}

TEST(InterpreterMachineStateTest, PipelineRefusesUnusedTerminalImportDomain) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  Image.ExceptionMetadata.ParseStatus = ExceptionParseStatus::Complete;
  Segment Code;
  Code.VA = 0x1000;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Code.Data = {0xb8, 7, 0, 0, 0, 0xc3};
  Code.Size = Code.FileSz = Code.Data.size();
  Image.Segments.push_back(std::move(Code));
  PipelineOptions Options;
  Options.OnlyFunctionEntries.insert(0x1000);
  Options.InterpreterSpecialization.emplace();
  Options.InterpreterSpecialization->ExplicitMachineState = true;
  Options.InterpreterSpecialization->NormalNonfaultingExecution = true;
  llvm::LLVMContext Context;
  Options.InterpreterSpecialization->X64TerminalImports = {
      {1, 0x3000, "fixture.dll", "StopFixture"}};
  Pipeline P;
  const auto Result = P.run(Image, Context, Options);
  EXPECT_FALSE(Result.Success);
  EXPECT_NE(Result.Error.find("terminal import domains"), std::string::npos);
  EXPECT_FALSE(Result.LlvmModule);
}

TEST(InterpreterMachineStateTest, PipelineRefusesUnusedRawEnvironmentDomains) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  Image.ExceptionMetadata.ParseStatus = ExceptionParseStatus::Complete;
  Segment Code;
  Code.VA = 0x1000;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Code.Data = {0xb8, 7, 0, 0, 0, 0xc3};
  Code.Size = Code.FileSz = Code.Data.size();
  Image.Segments.push_back(std::move(Code));
  for (bool Binding : {false, true}) {
    PipelineOptions Options;
    Options.OnlyFunctionEntries.insert(0x1000);
    Options.InterpreterSpecialization.emplace();
    if (Binding)
      Options.InterpreterSpecialization->X64ImportBindings.push_back(
          {1, 0x3000, "fixture.dll", "ReturnFixture", 0x4000, "test binding"});
    else
      Options.InterpreterSpecialization->X64CodeRegions.push_back(
          {1, 0x4000, {0xc3}, "test code"});
    llvm::LLVMContext Context;
    const auto R = Pipeline().run(Image, Context, Options);
    EXPECT_FALSE(R.Success);
    EXPECT_NE(R.Error.find("raw environment domains"), std::string::npos);
    EXPECT_FALSE(R.LlvmModule);
    EXPECT_FALSE(R.InterpreterRecovery);
  }
}

TEST(InterpreterMachineStateTest, RejectsUnsupportedEntryFlagsAndProfiles) {
  InterpreterMachineStateX64V1 State;
  EXPECT_FALSE(static_cast<bool>(validateInterpreterMachineStateX64V1(State)));
  for (unsigned Bit : {3u, 5u, 8u, 12u, 13u, 16u, 17u, 18u, 63u}) {
    State.RFlags = 2 | (uint64_t{1} << Bit);
    auto Error = validateInterpreterMachineStateX64V1(State);
    ASSERT_TRUE(static_cast<bool>(Error));
    llvm::consumeError(std::move(Error));
  }
  const auto Function = function({{operation(NdOp::RETURN, {}, {})}});
  auto Invalid = wrapInterpreterMachineStateX64(
      Function, BinaryFormat::ELF,
      static_cast<InterpreterMachineStateProfile>(255));
  EXPECT_FALSE(static_cast<bool>(Invalid));
  llvm::consumeError(Invalid.takeError());
}

TEST(InterpreterMachineStateTest, RejectsUnknownRegistersAndIntrinsics) {
  for (LowOp Op :
       {operation(NdOp::COPY, NdVar::reg(x86reg::XMM0, 8),
                  {NdVar::scalar(0, 8)}),
        operation(NdOp::INTRINSIC, {},
                  {NdVar::scalar(static_cast<uint64_t>(Intrinsic::Popf), 2),
                   NdVar::scalar(0x102, 8)})}) {
    auto Invalid = wrapInterpreterMachineStateX64(
        function({{Op}, {operation(NdOp::RETURN, {}, {})}}));
    EXPECT_FALSE(static_cast<bool>(Invalid));
    llvm::consumeError(Invalid.takeError());
  }
}

TEST(InterpreterMachineStateTest, GuestConstantsLoseSourceRelocationIdentity) {
  constexpr uint64_t Address = 0x62000000;
  const NdVar Constants[] = {NdVar::cst(Address, 8),
                             NdVar::scalar(Address, 8),
                             NdVar::addressFragment(Address, 8),
                             NdVar::address(Address, 8),
                             NdVar::dataAddress(Address, 8, Address),
                             NdVar::codeAddress(Address, 8, Address)};
  for (const NdVar &Constant : Constants) {
    SCOPED_TRACE(static_cast<unsigned>(Constant.Provenance));
    const auto Function = function(
        {{operation(NdOp::COPY, NdVar::reg(x86reg::RAX, 8), {Constant})},
         {operation(NdOp::RETURN, {}, {})}});
    auto Wrapped = wrapInterpreterMachineStateX64(Function);
    ASSERT_TRUE(static_cast<bool>(Wrapped))
        << llvm::toString(Wrapped.takeError());
    unsigned Observed = 0;
    for (const auto &Block : Wrapped->Function.Blocks)
      for (const auto &Op : Block.Ops)
        for (unsigned I = 0; I != Op.NumInputs; ++I)
          if (Op.Inputs[I].isConst() && Op.Inputs[I].Offset == Address) {
            ++Observed;
            EXPECT_EQ(Op.Inputs[I].Provenance,
                      ConstantAddressProvenance::Scalar);
            EXPECT_EQ(Op.Inputs[I].AddressOwnerVA, InvalidVA);
            EXPECT_EQ(Op.Inputs[I].Size, Constant.Size);
          }
    EXPECT_EQ(Observed, 1u);
    EXPECT_EQ(Function.Blocks.front().Ops.front().Inputs[0], Constant);
  }
}

TEST(InterpreterMachineStateTest, DirectDestinationsKeepTheirCFGIdentity) {
  const NdVar Destination = NdVar::codeAddress(0x2000, 8, 0x1000);
  auto Function = function({{operation(NdOp::BRANCH, {}, {Destination})}});
  auto &Entry = Function.Blocks.front();
  Entry.Succs = {1};
  auto &Boundary = Entry.InstructionBoundaries.front();
  Boundary.Control = LowInstructionControl::Branch;
  Boundary.ControlFlags = LowInstructionControlFlag::Branch;
  Boundary.Immediate = Destination.Offset;
  auto ExitFunction = function({{operation(NdOp::RETURN, {}, {})}});
  auto Exit = std::move(ExitFunction.Blocks.front());
  Exit.Id = 1;
  Exit.StartAddr = 0x2000;
  Exit.EndAddr = 0x2001;
  Exit.Preds = {0};
  Exit.Ops.front().Addr = 0x2000;
  Exit.InstructionBoundaries.front().Address = 0x2000;
  Function.Blocks.push_back(std::move(Exit));
  auto Wrapped = wrapInterpreterMachineStateX64(Function);
  ASSERT_TRUE(static_cast<bool>(Wrapped))
      << llvm::toString(Wrapped.takeError());
  const auto &Branch = Wrapped->Function.Blocks.front().Ops.back();
  EXPECT_EQ(Branch.Opcode, NdOp::BRANCH);
  ASSERT_EQ(Branch.NumInputs, 1u);
  EXPECT_EQ(Branch.Inputs[0], Destination);
  EXPECT_EQ(Wrapped->Function.Blocks.front().Succs, std::vector<int>{1});
}

TEST_F(InterpreterMachineSourceTest,
       FixedGuestAddressesRemainNumericThroughLoadsStoresAndState) {
#if !defined(__linux__) || !defined(__x86_64__)
  GTEST_SKIP() << "fixed guest mappings require a 64-bit Linux host";
#else
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "machine source execution requires clang";
  const auto RAX = NdVar::reg(x86reg::RAX, 8);
  const auto RCX = NdVar::reg(x86reg::RCX, 8);
  const auto RDX = NdVar::reg(x86reg::RDX, 8);
  const auto RBX = NdVar::reg(x86reg::RBX, 8);
  const auto Input = NdVar::dataAddress(0x62000000, 8, 0x62000000);
  const auto Output = NdVar::address(0x62000008, 8);
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  Segment Data;
  Data.Name = ".data";
  Data.VA = Input.Offset;
  Data.Size = 24;
  Data.FileSz = Data.Size;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.assign(Data.Size, 0xdd);
  Image.Segments.push_back(std::move(Data));
  Image.Symbols.push_back({"generic_guest_data", Input.Offset, 24, false});
  const auto Function = function(
      {{operation(NdOp::COPY, RCX, {Input}),
        operation(NdOp::COPY, RDX, {NdVar::codeAddress(0x420000, 8, 0x420000)}),
        operation(NdOp::COPY, RBX, {Output})},
       {operation(NdOp::LOAD, RAX, {Input}),
        operation(NdOp::INT_ADD, RAX, {RAX, NdVar::scalar(17, 8)}),
        operation(NdOp::STORE, {}, {Output, RAX})},
       {operation(NdOp::RETURN, {}, {RAX})}});
  roundTrip(Function, R"(
#include <stdint.h>
#include <sys/mman.h>
int main(void) {
  void *requested = (void *)(uintptr_t)UINT64_C(0x62000000);
  void *mapping = mmap(requested, 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mapping == MAP_FAILED) return 10;
  if (mapping != requested) { munmap(mapping, 4096); return 11; }
  uint64_t *memory = (uint64_t *)mapping;
  const uint64_t values[] = {0, 1, UINT64_MAX - 16, UINT64_MAX};
  for (unsigned sample = 0; sample != sizeof(values) / sizeof(values[0]);
       ++sample) {
    uint64_t state[17], before[17];
    for (unsigned i = 0; i != 16; ++i) state[i] = 100 + i;
    state[16] = 0x202;
    for (unsigned i = 0; i != 17; ++i) before[i] = state[i];
    memory[0] = values[sample];
    memory[1] = 0;
    memory[2] = UINT64_C(0x5a6b7c8d9eaf1021);
    if (generic_machine_source(MACHINE_ARG(state)) != 0) return 1;
    if (state[0] != values[sample] + 17 || memory[0] != values[sample] ||
        memory[1] != values[sample] + 17 ||
        memory[2] != UINT64_C(0x5a6b7c8d9eaf1021)) return 2;
    if (state[1] != UINT64_C(0x62000000) ||
        state[2] != UINT64_C(0x420000) ||
        state[3] != UINT64_C(0x62000008)) return 3;
    for (unsigned i = 4; i != 17; ++i)
      if (state[i] != before[i]) return 4;
  }
  return munmap(mapping, 4096) != 0;
}
)",
            "#define _GNU_SOURCE\n", &Image);
#endif
}

TEST_F(InterpreterMachineSourceTest,
       EntryFlagsAndGuestStackRemainExplicitAcrossBothSourceBackends) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "machine source execution requires clang";
  const auto RAX = NdVar::reg(x86reg::RAX, 8);
  const auto RSP = NdVar::reg(x86reg::RSP, 8);
  const auto Function =
      function({{operation(NdOp::INT_ZEXT, NdVar::tmp(0, 8),
                           {NdVar::reg(x86reg::CF, 1)}),
                 operation(NdOp::INT_ADD, RAX, {RAX, NdVar::tmp(0, 8)})},
                {operation(NdOp::INT_SUB, RSP, {RSP, NdVar::scalar(8, 8)}),
                 operation(NdOp::STORE, {}, {RSP, RAX})},
                {operation(NdOp::LOAD, NdVar::reg(x86reg::RBX, 8), {RSP}),
                 operation(NdOp::INT_ADD, RSP, {RSP, NdVar::scalar(8, 8)})},
                {operation(NdOp::RETURN, {}, {RAX})}});
  roundTrip(Function, R"(
#include <stdint.h>
int main(void) {
  const unsigned bits[] = {0,2,4,6,7,10,11};
  for (unsigned combination = 0; combination < 128; ++combination) {
    uint64_t flags = 0x202, memory = 0, state[17], before[17];
    for (unsigned i = 0; i < 7; ++i)
      flags |= ((uint64_t)((combination >> i) & 1)) << bits[i];
    for (unsigned i = 0; i < 16; ++i) state[i] = 100 + i;
    state[0] = UINT64_MAX - combination;
    state[4] = (uint64_t)(uintptr_t)(&memory + 1);
    state[16] = flags;
    for (unsigned i = 0; i < 17; ++i) before[i] = state[i];
    if (generic_machine_source(MACHINE_ARG(state)) != 0) return 1;
    uint64_t expected = before[0] + (flags & 1);
    if (state[0] != expected || state[3] != expected || memory != expected)
      return 2;
    for (unsigned i = 0; i < 17; ++i)
      if (i != 0 && i != 3 && state[i] != before[i]) return 3;
  }
  uint64_t invalid[17] = {0}, memory = 0;
  invalid[4] = (uint64_t)(uintptr_t)(&memory + 1);
  invalid[16] = 0x102;
  if (generic_machine_source(MACHINE_ARG(invalid)) == 0) return 4;
  return 0;
}
)");
}

TEST_F(InterpreterMachineSourceTest,
       PackedSystemFlagsUseUserModeWritesAndReportInvalidDynamicImages) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "machine source execution requires clang";
  const auto Function = function(
      {{operation(NdOp::INTRINSIC, {},
                  {NdVar::scalar(static_cast<uint64_t>(Intrinsic::Popf), 2),
                   NdVar::reg(x86reg::RCX, 8)})},
       {operation(NdOp::INTRINSIC, NdVar::tmp(0, 8),
                  {NdVar::scalar(static_cast<uint64_t>(Intrinsic::Pushf), 2)}),
        operation(NdOp::COPY, NdVar::reg(x86reg::RAX, 8), {NdVar::tmp(0, 8)})},
       {operation(NdOp::RETURN, {}, {})}});
  roundTrip(Function, R"(
#include <stdint.h>
int main(void) {
  uint64_t state[17] = {0};
  state[16] = 0x203;
  state[1] = 0x204002; /* User NT/ID write; IF is not user-writable. */
  if (generic_machine_source(MACHINE_ARG(state)) != 0) return 1;
  if (state[0] != 0x204202 || state[16] != 0x204203) return 2;
  state[1] = 0x102; /* TF requires a trap model and invalidates this run. */
  if (generic_machine_source(MACHINE_ARG(state)) == 0) return 3;
  state[1] = 0x40002; /* AC requires an alignment-exception model. */
  if (generic_machine_source(MACHINE_ARG(state)) == 0) return 4;
  return 0;
}
)");
}

TEST_F(InterpreterMachineSourceTest,
       PackedFlagsMatchIndependentUserModeOracle) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "machine source execution requires clang";
  const auto Function = function(
      {{operation(NdOp::INTRINSIC, {},
                  {NdVar::scalar(static_cast<uint64_t>(Intrinsic::Popf), 2),
                   NdVar::reg(x86reg::RCX, 8)})},
       {operation(NdOp::INTRINSIC, NdVar::tmp(0, 8),
                  {NdVar::scalar(static_cast<uint64_t>(Intrinsic::Pushf), 2)}),
        operation(NdOp::COPY, NdVar::reg(x86reg::RAX, 8), {NdVar::tmp(0, 8)})},
       {operation(NdOp::RETURN, {}, {})}});
  roundTrip(Function, R"(
#include <stdint.h>
int main(void) {
  const unsigned scalar_bits[] = {0, 2, 4, 6, 7, 10, 11};
  const uint64_t images[] = {0, 2, 0x4000, 0x200000, 0x204002,
                            0x1b3002, 0xfffffffffffbfeffULL, 0x102, 0x40002};
  for (unsigned system = 0; system != 8; ++system)
    for (unsigned scalar = 0; scalar != 128; ++scalar)
      for (unsigned i = 0; i != sizeof(images) / sizeof(images[0]); ++i) {
        uint64_t entry = 2 | ((system & 1) ? 0x200 : 0)
                           | ((system & 2) ? 0x4000 : 0)
                           | ((system & 4) ? 0x200000 : 0);
        for (unsigned bit = 0; bit != 7; ++bit)
          entry |= (uint64_t)((scalar >> bit) & 1) << scalar_bits[bit];
        uint64_t state[17] = {0};
        state[16] = entry;
        state[1] = images[i];
        const uint64_t status = generic_machine_source(MACHINE_ARG(state));
        if ((status != 0) != ((images[i] & 0x40100) != 0)) return 1;
        if (status != 0) continue;
        /* Independent CPL3/IOPL0 oracle: NT and ID writable, IF retained,
           fixed bit 1; this synthetic intrinsic leaves the seven scalar
           registers alone, as their pack/scatter belongs to the lifter. */
        const uint64_t expected_system = 2 | (entry & 0x200)
                                          | (images[i] & 0x204000);
        if (state[0] != expected_system) return 2;
        if (state[16] != (expected_system | (entry & 0xcd5))) return 3;
      }
  return 0;
}
)");
}

TEST_F(InterpreterMachineSourceTest, PackedFlagsProfileFailureStaysSticky) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "machine source execution requires clang";
  const auto Function = function(
      {{operation(NdOp::INTRINSIC, {},
                  {NdVar::scalar(static_cast<uint64_t>(Intrinsic::Popf), 2),
                   NdVar::reg(x86reg::RCX, 8)})},
       {operation(NdOp::INTRINSIC, {},
                  {NdVar::scalar(static_cast<uint64_t>(Intrinsic::Popf), 2),
                   NdVar::scalar(2, 8)})},
       {operation(NdOp::RETURN, {}, {})}});
  roundTrip(Function, R"(
#include <stdint.h>
int main(void) {
  const uint64_t inputs[] = {0, 2, 0x204002, 0x102, 0x40002, 0x40102};
  for (unsigned i = 0; i != sizeof(inputs) / sizeof(inputs[0]); ++i) {
    uint64_t state[17] = {0};
    state[16] = 0x202;
    state[1] = inputs[i];
    const uint64_t status = generic_machine_source(MACHINE_ARG(state));
    if ((status != 0) != ((inputs[i] & 0x40100) != 0)) return 1;
  }
  return 0;
}
)");
}

TEST_F(InterpreterMachineSourceTest, FunctionTemporarySurvivesSourceLowering) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "machine source execution requires clang";
  const auto Snapshot = NdVar::tmp(64, 8);
  auto F =
      function({{operation(NdOp::COPY, Snapshot, {NdVar::reg(x86reg::RCX, 8)})},
                {operation(NdOp::COPY, NdVar::reg(x86reg::RCX, 8),
                           {NdVar::scalar(0, 8)})},
                {operation(NdOp::COPY, NdVar::reg(x86reg::RAX, 8), {Snapshot})},
                {operation(NdOp::RETURN, {}, {})}});
  F.FunctionTemporaries = {{64, 8}};
  // Put the capture and its consumers in separate blocks.
  auto &Entry = F.Blocks.front();
  LowBlock Exit;
  Exit.Id = 1;
  Exit.StartAddr = Entry.InstructionBoundaries[1].Address;
  Exit.EndAddr = Entry.EndAddr;
  Exit.Preds = {0};
  Exit.Ops.assign(Entry.Ops.begin() + 1, Entry.Ops.end());
  Exit.InstructionBoundaries.assign(Entry.InstructionBoundaries.begin() + 1,
                                    Entry.InstructionBoundaries.end());
  for (auto &Boundary : Exit.InstructionBoundaries)
    --Boundary.FirstOp;
  Entry.Ops.resize(1);
  Entry.InstructionBoundaries.resize(1);
  Entry.EndAddr = Exit.StartAddr;
  Entry.Succs = {1};
  F.Blocks.push_back(std::move(Exit));
  roundTrip(F, R"(
#include <stdint.h>
int main(void) {
  uint64_t seed = UINT64_C(0x8123456789abcdef);
  for (unsigned k = 0; k != 256; ++k) {
    uint64_t state[17], expected[17];
    for (unsigned i = 0; i != 16; ++i) {
      seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
      state[i] = expected[i] = seed;
    }
    state[16] = expected[16] = 2;
    expected[0] = state[1]; expected[1] = 0;
    if (generic_machine_source(MACHINE_ARG(state))) return 1;
    for (unsigned i = 0; i != 17; ++i)
      if (state[i] != expected[i]) return 2;
  }
  return 0;
}
)");
}

TEST(InterpreterMachineStateTest, FunctionTemporaryByteAliasesRefuseSource) {
  for (auto Value : {NdVar::tmp(64, 4), NdVar::tmp(68, 4), NdVar::tmp(60, 8)}) {
    auto F = function(
        {{operation(NdOp::COPY, Value, {NdVar::scalar(7, Value.Size)})},
         {operation(NdOp::RETURN, {}, {})}});
    F.FunctionTemporaries = {{64, 8}};
    auto Source = wrapInterpreterMachineStateX64(F, BinaryFormat::ELF);
    ASSERT_FALSE(bool(Source));
    EXPECT_NE(llvm::toString(Source.takeError()).find("without byte aliases"),
              std::string::npos);
    // The byte-addressed proof model retains the exact partial write.
    auto Model = modelInterpreterMachineStateX64(F);
    ASSERT_TRUE(bool(Model)) << llvm::toString(Model.takeError());
  }
}

TEST_F(InterpreterMachineSourceTest, EveryGPRLaneSurvivesSyntheticRemapping) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "machine source execution requires clang";
  for (uint16_t Bytes : {1, 2, 4}) {
    SCOPED_TRACE(Bytes);
    std::vector<LowOp> Writes;
    for (uint64_t Register = 0; Register < 128; Register += 8)
      Writes.push_back(
          operation(NdOp::INT_ADD, NdVar::reg(Register, Bytes),
                    {NdVar::reg(Register, Bytes), NdVar::scalar(3, Bytes)}));
    auto Function = function({Writes, {operation(NdOp::RETURN, {}, {})}});
    // A wider read after a CFG edge must observe every partial write, including
    // SP/BP aliases. The entry-state wrapper, rather than the host stack, owns
    // their values in this LowIR source transition.
    auto &Entry = Function.Blocks.front();
    LowBlock Exit;
    Exit.Id = 1;
    Exit.StartAddr = Entry.InstructionBoundaries.back().Address;
    Exit.EndAddr = Entry.EndAddr;
    Exit.Preds = {0};
    Exit.Ops.push_back(Entry.Ops.back());
    Exit.InstructionBoundaries.push_back(Entry.InstructionBoundaries.back());
    Exit.InstructionBoundaries.front().FirstOp = 0;
    Entry.Ops.pop_back();
    Entry.InstructionBoundaries.pop_back();
    Entry.EndAddr = Exit.StartAddr;
    Entry.Succs = {1};
    Function.Blocks.push_back(std::move(Exit));
    const std::string Harness =
        "\n#include <stdint.h>\nint main(void) {\n"
        "for(unsigned k=0;k<8;++k){ uint64_t state[17], before[16];\n"
        "for(unsigned i=0;i<16;++i) before[i]=state[i]="
        "(UINT64_C(0x8123456789abcde0) ^ "
        "(i*UINT64_C(0x102030405060708)))+k;\n"
        "state[16]=2;\n"
        "if(generic_machine_source(MACHINE_ARG(state))) return 1;\n"
        "const uint64_t mask=(UINT64_C(1)<<" +
        std::to_string(Bytes * 8) +
        ")-1;\n"
        "for(unsigned i=0;i<16;++i){ uint64_t expected=(before[i]+3)&mask;\n" +
        (Bytes == 4 ? std::string{} : "expected|=before[i]&~mask;\n") +
        "if(state[i]!=expected) return 2;}\n"
        "if(state[16]!=2) return 3;\n}return 0;}\n";
    roundTrip(Function, Harness);
  }
}

TEST_F(InterpreterMachineSourceTest,
       HighByteReadsWritesAndOverlapsAreExplicit) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "machine source execution requires clang";
  std::vector<LowOp> Writes;
  for (uint64_t Register = 0; Register < 32; Register += 8)
    Writes.push_back(
        operation(NdOp::INT_ADD, NdVar::reg(Register + 1, 1),
                  {NdVar::reg(Register + 1, 1), NdVar::scalar(5, 1)}));
  Writes.push_back(operation(NdOp::COPY, NdVar::reg(x86reg::RAX, 2),
                             {NdVar::scalar(0xbeef, 2)}));
  Writes.push_back(operation(NdOp::COPY, NdVar::reg(x86reg::RAX, 1),
                             {NdVar::scalar(0xd3, 1)}));
  Writes.push_back(operation(NdOp::INT_ZEXT, NdVar::reg(x86reg::R8, 8),
                             {NdVar::reg(x86reg::RAX + 1, 1)}));
  roundTrip(function({Writes, {operation(NdOp::RETURN, {}, {})}}), R"(
#include <stdint.h>
int main(void) {
  for(unsigned k=0;k<8;++k){ uint64_t state[17], before[16];
    for(unsigned i=0;i<16;++i)
      before[i]=state[i]=(UINT64_C(0x8123456789abcde0)^i)+k*256;
    state[16]=2;
    if(generic_machine_source(MACHINE_ARG(state))) return 1;
    for(unsigned i=0;i<16;++i){
      uint64_t expected=before[i];
      if(i<4) expected=(expected&~UINT64_C(0xff00)) |
                      (((((before[i]>>8)&255)+5)&255)<<8);
      if(i==0) expected=(before[i]&~UINT64_C(0xffff))|0xbed3;
      if(i==8) expected=0xbe;
      if(state[i]!=expected) return 2;
    }
    if(state[16]!=2) return 3;
  }
  return 0;
}
)");
}

TEST_F(InterpreterMachineSourceTest, LowRegisterViewsDoNotBecomeNativeWrites) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "machine source execution requires clang";
  std::vector<LowOp> Views;
  for (uint64_t Register = 0; Register < 128; Register += 8)
    Views.push_back(operation(NdOp::SUBBYTES, NdVar::reg(Register, 4),
                              {NdVar::reg(Register, 8), NdVar::scalar(0, 4)}));
  roundTrip(function({Views, {operation(NdOp::RETURN, {}, {})}}), R"(
#include <stdint.h>
int main(void) {
  uint64_t state[17], before[16];
  for(unsigned i=0;i<16;++i)
    before[i]=state[i]=UINT64_C(0x8123456789abcde0)^i;
  state[16]=2;
  if(generic_machine_source(MACHINE_ARG(state))) return 1;
  for(unsigned i=0;i<16;++i) if(state[i]!=before[i]) return 2;
  return state[16]!=2;
}
)");
}

// The source fragment declares the shared fixture used by later fragments.
// clang-format off
#include "InterpreterMachineRegionSourceTests.inc"
#include "InterpreterMachineRegionChoiceTests.inc"
#include "InterpreterMachineRegionChoiceSourceTests.inc"
#include "InterpreterMachineRegionTrapTests.inc"
// clang-format on

} // namespace
