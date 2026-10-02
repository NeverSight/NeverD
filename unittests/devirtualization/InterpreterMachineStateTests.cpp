//===- InterpreterMachineStateTests.cpp - Recovery source ABI checks ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../lift/NeverDLiftFixture.h"

#include "neverd/analysis/InterpreterMachineState.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Error.h"

#include <map>

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
            std::optional<InterpreterEntryAlignment> Alignment = std::nullopt) {
    auto Wrapped = wrapInterpreterMachineStateX64(
        Function, BinaryFormat::ELF,
        InterpreterMachineStateProfile::UserX64NoFaultV1, Alignment);
    ASSERT_TRUE(static_cast<bool>(Wrapped))
        << llvm::toString(Wrapped.takeError());
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
      Options.Format = BinaryFormat::ELF;
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
                  "-fsanitize=undefined", "-fsanitize-trap=undefined",
                  SourceFile.string(), "-o", Program.string()});
        ASSERT_TRUE(Built.ok()) << Built.err << '\n' << Source;
        const auto Ran = exec(Program.string(), {});
        EXPECT_TRUE(Ran.ok()) << Ran.err << " exit " << Ran.exitCode << '\n'
                              << Source;
      }
    }
  }
};

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

} // namespace
