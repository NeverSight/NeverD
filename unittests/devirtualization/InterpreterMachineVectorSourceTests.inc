//===- InterpreterMachineVectorSourceTests.inc - XMM source state tests
//----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent whole-state checks for vector and ordered-query source output.
//===----------------------------------------------------------------------===//

TEST_F(InterpreterMachineSourceTest,
       VectorStatePreservesBytesAndQueryFailures) {
  const auto Query = cpuQueryInstruction();
  ASSERT_EQ(Query.size(), 9u);
  const auto F = function(
      {{operation(NdOp::COPY, NdVar::tmp(TmpBase, 8),
                  {NdVar::scalar(0x0706050403020100, 8)}),
        operation(NdOp::COPY, NdVar::tmp(TmpBase + 8, 8),
                  {NdVar::scalar(0x0f0e0d0c0b0a0908, 8)}),
        operation(NdOp::COPY, NdVar::tmp(TmpBase + 3, 1),
                  {NdVar::scalar(0xee, 1)}),
        operation(NdOp::COPY, NdVar::reg(x86reg::XMM0, 16),
                  {NdVar::tmp(TmpBase, 16)}),
        operation(NdOp::COPY, NdVar::tmp(TmpBase, 8),
                  {NdVar::scalar(0xaaaaaaaaaaaaaaaa, 8)}),
        operation(NdOp::COPY, NdVar::reg(x86reg::XMM15, 16),
                  {NdVar::reg(x86reg::XMM0, 16)}),
        operation(NdOp::STORE, {},
                  {NdVar::reg(x86reg::RDI, 8), NdVar::reg(x86reg::XMM0, 16)}),
        operation(NdOp::COPY, NdVar::reg(x86reg::RAX, 8),
                  {NdVar::scalar(7, 8)}),
        operation(NdOp::COPY, NdVar::reg(x86reg::RCX, 8),
                  {NdVar::scalar(3, 8)})},
       Query,
       {operation(NdOp::RETURN, {}, {})}});
  roundTrip(F, R"C(
#include <stdint.h>
#include <string.h>
int main(void) {
  for (unsigned mode=0; mode<4; ++mode) {
    _Alignas(16) uint64_t storage[54], before[54], expected[52];
    uint64_t *state=storage+1;
    uint8_t memory[64], want[64], vector[16];
    uint32_t tape[6]={7,3,0xffffffffu,0x80000000u,0x12345678u,0x87654321u};
    for (unsigned i=0;i<54;++i) storage[i]=0x1234567800000000ull+i;
    for (unsigned i=0;i<16;++i) vector[i]=(uint8_t)i;
    vector[3]=0xee;
    memset(memory,0xa7,sizeof memory); memcpy(want,memory,sizeof memory);
    state[7]=(uintptr_t)(memory+3); state[4]=0x123458;
    state[16]=0x246; state[17]=(uintptr_t)tape; state[18]=1; state[19]=0;
    if (mode==1) {state[17]=0;state[18]=0;}
    if (mode==2) tape[1]=4;
    if (mode==3) state[4]+=1;
    memcpy(before,storage,sizeof storage); memcpy(expected,state,sizeof expected);
    unsigned status=mode==1?3:mode==2?4:mode==3?2:0;
    if (mode!=3) {
      expected[0]=7; expected[1]=3;
      memcpy(expected+20,vector,16); memcpy(expected+50,vector,16);
      memcpy(want+3,vector,16);
      if (!mode) {
        expected[0]=tape[2]; expected[3]=tape[3];
        expected[1]=tape[4]; expected[2]=tape[5]; expected[19]=1;
      }
    }
    if (generic_machine_source(MACHINE_ARG(state))!=status) return 1;
    if (memcmp(state,expected,sizeof expected)) return 2;
    if (memcmp(memory,want,sizeof memory)) return 3;
    if (storage[0]!=before[0] || storage[53]!=before[53]) return 4;
    if (tape[0]!=7 || tape[1]!=(mode==2?4u:3u) || tape[2]!=0xffffffffu ||
        tape[3]!=0x80000000u || tape[4]!=0x12345678u || tape[5]!=0x87654321u) return 5;
  }
  return 0;
}
)C",
            {}, nullptr, InterpreterEntryAlignment{16, 8},
            InterpreterMachineStateLayout::InlineReturns, true, true);
  auto Old = wrapInterpreterMachineQueryStateX64(F);
  EXPECT_FALSE(bool(Old));
  if (!Old)
    llvm::consumeError(Old.takeError());
}

TEST(InterpreterMachineState, VectorStateRejectsUnboundAndUnownedBytes) {
  for (unsigned Kind = 0; Kind != 8; ++Kind) {
    auto Output = NdVar::reg(x86reg::XMM0, 16);
    auto Input = NdVar::reg(x86reg::XMM1, 16);
    if (Kind == 0)
      Output.Offset = x86reg::XMM16;
    else if (Kind == 1)
      Output.Size = 32;
    else if (Kind == 2)
      Output.Size = 8;
    else if (Kind == 3)
      Output.Offset += 8;
    else
      Input = NdVar::tmp(TmpBase, 16);
    std::vector<LowOp> Ops;
    if (Kind >= 5)
      Ops.push_back(
          operation(NdOp::COPY, NdVar::tmp(TmpBase, 8), {NdVar::scalar(1, 8)}));
    Ops.push_back(operation(NdOp::COPY, Output, {Input}));
    auto F = function({Ops, {operation(NdOp::RETURN, {}, {})}});
    if (Kind == 6)
      F.FunctionTemporaries = {{TmpBase, 16}};
    if (Kind == 7)
      F.Blocks[0].Ops[0].Output.Offset = (uint64_t{1} << 61);
    auto Result = wrapInterpreterMachineQueryStateX64V2(F);
    EXPECT_FALSE(bool(Result)) << Kind;
    if (!Result)
      llvm::consumeError(Result.takeError());
  }
}

TEST_F(InterpreterMachineSourceTest, VectorStateWrapsPackedByteRecovery) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  Image.ExceptionMetadata.ParseStatus = ExceptionParseStatus::Complete;
  Segment Code;
  Code.VA = 0x1000;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  // MOVDQU XMM0,[RDI]; PADDB XMM0,XMM1; MOVDQU [RSI],XMM0; RET.
  Code.Data = {0xf3, 0x0f, 0x6f, 0x07, 0x66, 0x0f, 0xfc,
               0xc1, 0xf3, 0x0f, 0x7f, 0x06, 0xc3};
  Code.Size = Code.FileSz = Code.Data.size();
  Image.Segments.push_back(std::move(Code));
  SpecializationOptions Options;
  Options.ExplicitMachineState = true;
  Options.NormalNonfaultingExecution = true;
  Options.X64CetDisabled = true;
  Options.DiscoverControlState = true;
  Options.FrameBaseRegister = symbolic::SymRegisterRange{x86reg::RSP, 8};
  Options.RequireRestoredFrameAtReturn = true;
  Options.ExternalStoresPreserveEntryReturnSlot = true;
  auto Recovered = specializeBinaryInterpreter(Image, 0x1000, Options);
  ASSERT_TRUE(Recovered.complete()) << Recovered.Diagnostic;
  Recovered.Residual.Name = "generic_machine_source";
  roundTrip(Recovered.Residual, R"C(
#include <stdint.h>
#include <string.h>
int main(void) {
  for (unsigned scenario=0; scenario<32; ++scenario) {
    uint64_t state[52], expected[52];
    uint8_t memory[96], want[96], input[16], add[16], answer[16];
    for (unsigned i=0;i<52;++i) state[i]=0xabcdef0102030000ull+i*0x01010101;
    for (unsigned i=0;i<96;++i) memory[i]=(uint8_t)(scenario*19+i*7);
    for (unsigned i=0;i<16;++i) add[i]=(uint8_t)(scenario*29+i*13);
    unsigned destination=scenario%3==0?5:scenario%3==1?9:53;
    memcpy(input,memory+5,16); memcpy(state+22,add,16);
    for (unsigned i=0;i<16;++i) answer[i]=(uint8_t)(input[i]+add[i]);
    state[7]=(uintptr_t)(memory+5); state[6]=(uintptr_t)(memory+destination);
    state[16]=0x246; state[17]=0; state[18]=0; state[19]=0;
    memcpy(expected,state,sizeof state); memcpy(expected+20,answer,16);
    memcpy(want,memory,sizeof memory); memcpy(want+destination,answer,16);
    if (generic_machine_source(MACHINE_ARG(state))) return 1;
    if (memcmp(state,expected,sizeof state)) return 2;
    if (memcmp(memory,want,sizeof memory)) return 3;
  }
  return 0;
}
)C",
            {}, nullptr, std::nullopt,
            InterpreterMachineStateLayout::InlineReturns, false, true);
}

TEST_F(InterpreterMachineSourceTest, VectorStateJoinsLoopsAndSharedReturns) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  Image.ExceptionMetadata.ParseStatus = ExceptionParseStatus::Complete;
  Segment Code;
  Code.VA = 0x1000;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  // Save flags; select XMM1 or XMM2; add XMM1 bytes RCX times; restore flags
  // at two distinct returns. Neither the source oracle nor the host runs these
  // native bytes: the expected bytes use independent unsigned arithmetic.
  Code.Data = {0x9c, 0x45, 0x85, 0xc0, 0x74, 0x06, 0xf3, 0x0f, 0x6f,
               0xc2, 0xeb, 0x04, 0xf3, 0x0f, 0x6f, 0xc1, 0x66, 0x0f,
               0xfc, 0xc1, 0x48, 0xff, 0xc9, 0x75, 0xf7, 0x45, 0x85,
               0xc0, 0x74, 0x02, 0x9d, 0xc3, 0x9d, 0xc3};
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
  const auto Harness = R"C(
#include <stdint.h>
#include <string.h>
int main(void) {
  for (unsigned choose=0; choose<2; ++choose)
    for (unsigned count=1; count<=5; ++count) {
      uint64_t state[52], expected[52];
      _Alignas(16) uint8_t stack[128], want[128];
      uint8_t first[16], second[16], answer[16];
      for (unsigned i=0;i<52;++i) state[i]=0xabcdef1234560000ull+i;
      for (unsigned i=0;i<128;++i) stack[i]=(uint8_t)(i*17+count);
      for (unsigned i=0;i<16;++i) {
        first[i]=(uint8_t)(count*13+i*29); second[i]=(uint8_t)(i*7+choose);
        answer[i]=(uint8_t)((choose?second[i]:first[i])+count*first[i]);
      }
      memcpy(state+22,first,16); memcpy(state+24,second,16);
      state[1]=count; state[4]=(uintptr_t)(stack+88); state[8]=choose;
      state[16]=choose?0x203:0x246; state[17]=0; state[18]=0; state[19]=0;
      memcpy(expected,state,sizeof state); expected[1]=0;
      memcpy(expected+20,answer,16); memcpy(want,stack,sizeof stack);
      memcpy(want+80,state+16,8);
      if (generic_machine_source(MACHINE_ARG(state))) return 1;
      if (memcmp(state,expected,sizeof state)) return 2;
      if (memcmp(stack,want,sizeof stack)) return 3;
    }
  return 0;
}
)C";
  for (auto Format :
       {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
    for (auto Layout : {InterpreterMachineStateLayout::InlineReturns,
                        InterpreterMachineStateLayout::SharedGPRExit})
      roundTrip(Recovered.Residual, Harness, {}, nullptr,
                InterpreterEntryAlignment{16, 8}, Layout, false, true, Format);
}

TEST(InterpreterMachineState, VectorStateRequiresProvedConstantAlignment) {
  for (unsigned Kind = 0; Kind != 4; ++Kind) {
    auto Address = Kind == 2 ? NdVar::reg(x86reg::RDI, 8)
                             : NdVar::scalar(Kind == 1 ? 0x1001 : 0x1000, 8);
    const auto F = function(
        {{operation(
             NdOp::INTRINSIC, {},
             {NdVar::cst(static_cast<uint64_t>(Intrinsic::RequireAligned), 2),
              Address, NdVar::scalar(Kind == 3 ? 3 : 16, 8)})},
         {operation(NdOp::RETURN, {}, {})}});
    auto Result = wrapInterpreterMachineQueryStateX64V2(F);
    EXPECT_EQ(bool(Result), Kind == 0) << Kind;
    if (!Result)
      llvm::consumeError(Result.takeError());
  }
}
