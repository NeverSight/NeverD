//===- BinaryUndefinedIndependenceTests.cpp - Lifted arbitrary bits
//--------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/analysis/LowIRUndefinedIndependence.h"
#include "neverd/decode/Decoder.h"
#include "neverd/lift/X86/X86Regs.h"

#include <array>

using namespace neverd;
using namespace neverd::analysis;

namespace {
using Status = LowIRIndependenceStatus;

struct BinaryProgram {
  Decoder Decode;
  LowFunc Function;
  std::vector<LowIRUndefinedInstruction> Records;
  LowIRIndependenceContract Contract;

  BinaryProgram() {
    EXPECT_TRUE(Decode.init(Arch::X64));
    Decode.setStrict(true);
    Function.Entry = 0x100;
    Contract.ReturnRegisters = {{x86reg::RAX, 8}};
  }

  void block(va_t Address, std::initializer_list<uint8_t> Bytes,
             std::vector<int> Successors = {}) {
    LowBlock Block;
    Block.Id = static_cast<int>(Function.Blocks.size());
    Block.StartAddr = Address;
    Block.EndAddr = Address + Bytes.size();
    Block.Succs = std::move(Successors);
    size_t Offset = 0;
    while (Offset < Bytes.size()) {
      DecodedInsn Insn{};
      const int Size =
          Decode.decodeOneForLift(Bytes.begin() + Offset, Bytes.size() - Offset,
                                  Address + Offset, Insn);
      ASSERT_GT(Size, 0);
      LowIRUndefinedInstruction Record;
      Record.BlockId = Block.Id;
      auto &Boundary = Record.Boundary;
      Boundary.Address = Insn.Addr;
      Boundary.Size = Insn.Size;
      Boundary.FirstOp = Block.Ops.size();
      ASSERT_NO_THROW(
          Decode.liftToLow(Insn, Block.Ops, {}, {}, &Record.Effects));
      Boundary.OpCount = Block.Ops.size() - Boundary.FirstOp;
      for (size_t I = Boundary.FirstOp; I < Block.Ops.size(); ++I) {
        const auto &Op = Block.Ops[I];
        if (Op.Opcode == NdOp::RETURN) {
          Boundary.Control = LowInstructionControl::Return;
          Boundary.ControlFlags = LowInstructionControlFlag::Return;
        } else if (Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR) {
          Boundary.Control = LowInstructionControl::Branch;
          Boundary.ControlFlags = LowInstructionControlFlag::Branch;
          if (Op.Opcode == NdOp::COND_BR)
            Boundary.ControlFlags |= LowInstructionControlFlag::Conditional;
          ASSERT_TRUE(Op.Inputs[0].isConst());
          Boundary.Immediate = Op.Inputs[0].Offset;
        }
      }
      Block.InstructionBoundaries.push_back(Boundary);
      Records.push_back(std::move(Record));
      Offset += Size;
    }
    Function.Blocks.push_back(std::move(Block));
  }

  void expect(Status Expected) {
    const auto Result =
        checkLowIRUndefinedIndependence(Function, Records, Contract);
    EXPECT_EQ(Result.Status, Expected) << Result.Diagnostic;
    EXPECT_EQ(Result.Certificate.has_value(), Expected == Status::Proved);
  }
};

TEST(BinaryUndefinedIndependence,
     LogicResultIsIndependentButAuxiliaryCarryIsNot) {
  BinaryProgram Program;
  Program.block(0x100, {0x31, 0xc0, 0xc3}); // xor eax,eax; ret
  Program.expect(Status::Proved);
  Program.Contract.ReturnRegisters.push_back({x86reg::AF, 1});
  Program.expect(Status::Dependent);
}

TEST(BinaryUndefinedIndependence,
     BMILogicDoesNotCertifyArbitraryAuxiliaryCarryOrParity) {
  for (auto Bytes : {std::array<uint8_t, 5>{0xc4, 0xe2, 0xf0, 0xf2, 0xc2},
                     std::array<uint8_t, 5>{0xc4, 0xe2, 0xf8, 0xf3, 0xd9},
                     std::array<uint8_t, 5>{0xc4, 0xe2, 0xf8, 0xf3, 0xd1},
                     std::array<uint8_t, 5>{0xc4, 0xe2, 0xf8, 0xf3, 0xc9},
                     std::array<uint8_t, 5>{0xc4, 0xe2, 0xe8, 0xf5, 0xc1}}) {
    BinaryProgram Program;
    Program.block(0x100,
                  {Bytes[0], Bytes[1], Bytes[2], Bytes[3], Bytes[4], 0xc3});
    for (unsigned Flag : {x86reg::CF, x86reg::ZF, x86reg::SF, x86reg::OF})
      Program.Contract.ReturnRegisters.push_back({Flag, 1});
    Program.expect(Status::Proved);
    for (unsigned Flag : {x86reg::AF, x86reg::PF}) {
      Program.Contract.ReturnRegisters.push_back({Flag, 1});
      Program.expect(Status::Dependent);
      Program.Contract.ReturnRegisters.pop_back();
    }
  }
}

TEST(BinaryUndefinedIndependence,
     BMILogicNFTransportsThePriorArbitraryAuxiliaryCarry) {
  for (auto Tail : {std::array<uint8_t, 4>{0xf4, 0x0c, 0xf2, 0xc2},
                    std::array<uint8_t, 4>{0xfc, 0x0c, 0xf3, 0xd9},
                    std::array<uint8_t, 4>{0xfc, 0x0c, 0xf3, 0xd1},
                    std::array<uint8_t, 4>{0xfc, 0x0c, 0xf3, 0xc9},
                    std::array<uint8_t, 4>{0xec, 0x0c, 0xf5, 0xc1}}) {
    BinaryProgram Program;
    Program.block(0x100, {0x31, 0xd2, 0x62, 0xf2, Tail[0], Tail[1], Tail[2],
                          Tail[3], 0xc3});
    Program.expect(Status::Proved);
    Program.Contract.ReturnRegisters.push_back({x86reg::AF, 1});
    Program.expect(Status::Dependent);
  }
}

TEST(BinaryUndefinedIndependence,
     BMI2ArithmeticTransportsThePriorArbitraryAuxiliaryCarry) {
  for (bool Apx : {false, true})
    for (auto Encoding : {std::array<uint8_t, 4>{0xfb, 0xf6, 0xf9, 2},
                          std::array<uint8_t, 4>{0xf3, 0xf7, 0xc1, 2},
                          std::array<uint8_t, 4>{0xf1, 0xf7, 0xc1, 2},
                          std::array<uint8_t, 4>{0xf2, 0xf7, 0xc1, 2},
                          std::array<uint8_t, 4>{0xfb, 0xf0, 0xc1, 3}}) {
      BinaryProgram Program;
      const auto [P1, Opcode, ModRM, Map] = Encoding;
      const uint8_t P0 = (Apx ? 0xf0 : 0xe0) | Map;
      // XOR defines the input but leaves AF arbitrary. RORX gets imm8=NOP;
      // the other forms consume that same byte as a separate NOP instruction.
      if (Apx)
        Program.block(0x100,
                      {0x31, 0xc9, 0x62, P0, static_cast<uint8_t>(P1 | 4), 8,
                       Opcode, ModRM, 0x90, 0xc3});
      else
        Program.block(0x100,
                      {0x31, 0xc9, 0xc4, P0, P1, Opcode, ModRM, 0x90, 0xc3});
      Program.expect(Status::Proved);
      Program.Contract.ReturnRegisters.push_back({x86reg::AF, 1});
      Program.expect(Status::Dependent);
    }
}

TEST(BinaryUndefinedIndependence,
     ADXConsumesOnlyItsOwnCarryAndPreservesOtherArbitraryFlags) {
  for (bool Overflow : {false, true}) {
    BinaryProgram Program;
    // TZCNT defines CF and leaves OF arbitrary. Only ADOX consumes that OF.
    Program.block(0x100, {0x31, 0xd2, 0xf3, 0x48, 0x0f, 0xbc, 0xca,
                          static_cast<uint8_t>(Overflow ? 0xf3 : 0x66), 0x48,
                          0x0f, 0x38, 0xf6, 0xc1, 0xc3});
    Program.expect(Overflow ? Status::Dependent : Status::Proved);
    if (!Overflow) {
      Program.Contract.ReturnRegisters.push_back({x86reg::OF, 1});
      Program.expect(Status::Dependent);
    }
  }
  for (uint8_t PP : {1, 2}) {
    BinaryProgram Program;
    Program.block(0x100,
                  {0x31, 0xc9, 0x62, 0xf4, static_cast<uint8_t>(0xfc | PP), 8,
                   0x66, 0xc1, 0xc3});
    Program.expect(Status::Proved);
    Program.Contract.ReturnRegisters.push_back({x86reg::AF, 1});
    Program.expect(Status::Dependent);
  }
}

TEST(BinaryUndefinedIndependence,
     BitCountsDoNotCertifyUndefinedStatusFlagsAsConstants) {
  for (uint8_t Opcode : {0xbc, 0xbd}) {
    BinaryProgram Program;
    Program.block(0x100, {0xf3, 0x48, 0x0f, Opcode, 0xc1, 0xc3});
    Program.Contract.ReturnRegisters.push_back({x86reg::CF, 1});
    Program.Contract.ReturnRegisters.push_back({x86reg::ZF, 1});
    Program.expect(Status::Proved);
    for (unsigned Flag : {x86reg::OF, x86reg::SF, x86reg::AF, x86reg::PF}) {
      Program.Contract.ReturnRegisters.push_back({Flag, 1});
      Program.expect(Status::Dependent);
      Program.Contract.ReturnRegisters.pop_back();
    }
  }
}

TEST(BinaryUndefinedIndependence,
     BitCountNFTransportsThePriorArbitraryAuxiliaryCarry) {
  for (uint8_t Opcode : {0xf4, 0xf5, 0x88}) {
    BinaryProgram Program;
    // XOR creates arbitrary AF; APX NF must retain that exact producer.
    Program.block(0x100,
                  {0x31, 0xd2, 0x62, 0xf4, 0xfc, 0x0c, Opcode, 0xc1, 0xc3});
    Program.expect(Status::Proved);
    Program.Contract.ReturnRegisters.push_back({x86reg::AF, 1});
    Program.expect(Status::Dependent);
  }
  BinaryProgram Defined;
  // Ordinary POPCNT defines AF and therefore kills the earlier arbitrary bit.
  Defined.block(0x100, {0x31, 0xd2, 0xf3, 0x48, 0x0f, 0xb8, 0xc1, 0xc3});
  Defined.Contract.ReturnRegisters.push_back({x86reg::AF, 1});
  Defined.expect(Status::Proved);
}

TEST(BinaryUndefinedIndependence, FlagSnapshotsShareOnlyTheirActualProducer) {
  BinaryProgram Shared;
  // xor ecx,ecx; lahf; mov dl,ah; lahf; xor dl,ah; movzx eax,dl; ret.
  Shared.block(0x100, {0x31, 0xc9, 0x9f, 0x88, 0xe2, 0x9f, 0x30, 0xe2, 0x0f,
                       0xb6, 0xc2, 0xc3});
  Shared.expect(Status::Proved);

  BinaryProgram Fresh;
  // An intervening XOR produces a new AF before the second LAHF snapshot.
  Fresh.block(0x100, {0x31, 0xc9, 0x9f, 0x88, 0xe2, 0x31, 0xc9, 0x9f, 0x30,
                      0xe2, 0x0f, 0xb6, 0xc2, 0xc3});
  Fresh.expect(Status::Dependent);
}

TEST(BinaryUndefinedIndependence, SnapshotSpillSurvivesUntilOverwritten) {
  BinaryProgram Spill;
  // xor eax,eax; lahf; mov [rsp-8],ah; mov cl,[rsp-8]; xor cl,ah;
  // movzx eax,cl; ret. The return is zero, but the spill still exposes AF.
  Spill.block(0x100, {0x31, 0xc0, 0x9f, 0x88, 0x64, 0x24, 0xf8, 0x8a, 0x4c,
                      0x24, 0xf8, 0x30, 0xe1, 0x0f, 0xb6, 0xc1, 0xc3});
  Spill.Contract.Frame = LowIRIndependenceFrame{{x86reg::RSP, 8}, -16, 8};
  Spill.expect(Status::Dependent);
  Spill.Contract.ObserveWrittenFrameBytes = false;
  Spill.expect(Status::Proved);

  BinaryProgram Killed;
  // Same program, with mov byte ptr [rsp-8],0 before returning.
  Killed.block(0x100, {0x31, 0xc0, 0x9f, 0x88, 0x64, 0x24, 0xf8, 0x8a,
                       0x4c, 0x24, 0xf8, 0x30, 0xe1, 0x0f, 0xb6, 0xc1,
                       0xc6, 0x44, 0x24, 0xf8, 0x00, 0xc3});
  Killed.Contract.Frame = Spill.Contract.Frame;
  Killed.expect(Status::Proved);
}

TEST(BinaryUndefinedIndependence,
     OriginalBranchMustNotAssumeItsUndefinedGuard) {
  BinaryProgram Program;
  // xor eax,eax; lahf; test ah,0x10; jnz 0x110.
  Program.block(0x100, {0x31, 0xc0, 0x9f, 0xf6, 0xc4, 0x10, 0x75, 0x08},
                {1, 2});
  Program.block(0x108, {0xb8, 0, 0, 0, 0, 0xc3});
  Program.block(0x110, {0xb8, 0, 0, 0, 0, 0xc3});
  Program.expect(Status::Dependent);

  BinaryProgram Defined;
  // test edi,edi; jnz 0x110. Ordinary entry inputs remain shared.
  Defined.block(0x100, {0x85, 0xff, 0x75, 0x0c}, {1, 2});
  Defined.block(0x104, {0xb8, 0, 0, 0, 0, 0xc3});
  Defined.block(0x110, {0xb8, 1, 0, 0, 0, 0xc3});
  Defined.expect(Status::Proved);
}

TEST(BinaryUndefinedIndependence,
     UncoveredNativeInstructionRefusesCertificate) {
  BinaryProgram Program;
  Program.block(0x100, {0xd1, 0xd0, 0xc3}); // rcl eax,1; ret
  Program.expect(Status::Unsupported);
}
TEST(BinaryUndefinedIndependence, ShiftGuardsBindTheCompleteOperationDigest) {
  BinaryProgram P;
  P.block(0x100, {0xd2, 0xe0, 0xc3}); // shl al,cl; ret.
  P.expect(Status::Proved);
  const auto &Effect = P.Records.front().Effects.Effects.front();
  ASSERT_TRUE(Effect.When.has_value());
  const auto After = Effect.AfterOp;
  ASSERT_GT(After, 0u);
  auto &Guard = P.Function.Blocks.front().Ops[After - 1];
  ASSERT_EQ(Guard.Opcode, NdOp::INT_LESSEQUAL);
  Guard.Inputs[0] = NdVar::scalar(31, Guard.Inputs[0].Size);
  P.expect(Status::Invalid);
}

} // namespace
