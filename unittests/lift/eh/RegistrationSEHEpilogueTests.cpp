//===- RegistrationSEHEpilogueTests.cpp - Original parent return proof ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check parent epilogue bytes, frame replay and retained MedIR exit evidence.

#include "../../../lib/ir/low/X86/RegistrationSEHControl.h"
#include "gtest/gtest.h"

#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/X86/X86RegistrationSEHParentExit.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/STLExtras.h"

using namespace neverd;

namespace {

BinaryImage epilogueImage(bool Scratch, unsigned Mutation) {
  BinaryImage Image;
  Image.Arch = Arch::X86;
  Image.Bits = Bitness::Bits32;
  Image.Format = BinaryFormat::COFF;
  Image.Base = 0x400000;
  Segment Text;
  Text.VA = 0x401000;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  const auto Emit = [&](std::initializer_list<uint8_t> Bytes) {
    const va_t Start = Text.VA + Text.Data.size();
    Text.Data.insert(Text.Data.end(), Bytes);
    return Start;
  };
  Emit({0x55, 0x8b, 0xec, 0x83, 0xec, 0x10});
  Emit({0x64, 0xa1, 0, 0, 0, 0, 0x89, 0x45, 0xf0});
  const va_t Install = Emit({0x64, 0x89, 0x25, 0, 0, 0, 0});
  Emit({0x83, 0xec, 0x0c, 0x53, 0x56, 0x57});
  if (Scratch)
    Emit({0x6a, 0});
  Emit({0x89, 0x65, 0xe8});
  if (Mutation == 1)
    Emit({0xc7, 0x45, 0x04, 0, 0, 0, 0}); // Caller return PC.
  if (Mutation == 2)
    Emit({0xc7, 0x45, 0x00, 0, 0, 0, 0}); // Saved EBP.
  if (Mutation == 3)
    Emit({0xc7, 0x45, 0xe0, 0, 0, 0, 0}); // Saved EBX.
  const va_t Active = Emit({0xc7, 0x45, 0xfc, 0, 0, 0, 0});
  const va_t Retired = Emit({0xc7, 0x45, 0xfc, 0xff, 0xff, 0xff, 0xff});
  Emit({0xe8, 0, 0, 0, 0});
  const va_t Finally = Emit({0xb8, 0x7b, 0, 0, 0});
  Emit({0x8b, 0x65, uint8_t(Mutation == 4 ? 0xe4 : 0xe8)});
  Emit({0x8b, 0x4d, 0xf0});
  Emit({0x64, 0x89, 0x0d, 0, 0, 0, 0});
  const va_t Epilogue = Text.VA + Text.Data.size();
  if (Mutation == 5)
    Emit({0xc3}); // A bare RET never establishes parent ownership.
  else {
    if (Scratch)
      Emit({0x59});
    Emit({0x5f, 0x5e, uint8_t(Mutation == 6 ? 0x59 : 0x5b)});
    Emit({0x8b, 0xe5, 0x5d, 0xc3});
  }
  Text.Size = Text.FileSz = Text.Data.size();
  Image.Segments.push_back(Text);
  Section Code;
  Code.VA = Text.VA;
  Code.Size = Code.FileSz = Text.Size;
  Code.Flags = Text.Flags;
  Image.Sections.push_back(Code);
  ExceptionFunction EH;
  EH.CodeRange = {Text.VA, Text.VA + Text.Size};
  EH.Encoding = ExceptionEncoding::X86ScopeTableEH3;
  EH.Personality = ExceptionPersonality::ExceptHandler3;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  auto &Chain = EH.Registration.emplace();
  Chain.SeededTryLevel = -1;
  Chain.TryLevelOffset = -4;
  Chain.RegistrationOffset = -16;
  Chain.ChainInstallVA = Install;
  Chain.Scopes = {{-1, 0, Finally, true}};
  Chain.TryLevelStores = {{Active, Active + 7, 0}, {Retired, Retired + 7, -1}};
  if (Mutation == 7)
    Chain.Scopes.push_back({-1, 0, Epilogue, true});
  Image.ExceptionMetadata.Functions.push_back(EH);
  Image.ExceptionMetadata.rebuildIndex();
  return Image;
}

} // namespace

TEST(RegistrationSEHEpilogue,
     FallthroughRestoresOriginalCallerOnEveryInvocation) {
  for (bool Scratch : {false, true})
    for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
      SCOPED_TRACE(Scratch);
      SCOPED_TRACE(Mutation);
      auto Image = epilogueImage(Scratch, Mutation);
      Decoder Decode;
      ASSERT_TRUE(Decode.init(Image));
      const auto Function = CFGBuilder().build(Image, Decode, 0x401000);
      ASSERT_TRUE(Function.RegistrationStates);
      const auto &State = *Function.RegistrationStates;
      EXPECT_EQ(State.Complete && State.CallbackStatesComplete &&
                    State.RegistrationLifetimeComplete &&
                    State.SEHInvocationExitsComplete,
                Mutation == 0);
      if (Mutation)
        continue;
      ASSERT_EQ(State.SEHInvocationExits.size(), 2u);
      EXPECT_TRUE(llvm::all_of(State.SEHInvocationExits, [](const auto &Exit) {
        return Exit.Exit ==
                   RegistrationSEHInvocationExit::Kind::ParentFallthrough &&
               Exit.TargetVA == Exit.EndAddress;
      }));
      EXPECT_EQ(State.SEHInvocationExits[0].CallVA, 0u);
      EXPECT_NE(State.SEHInvocationExits[1].CallVA, 0u);
      LowToMedConverter Converter;
      Converter.setBinaryImage(&Image);
      const auto Med =
          Converter.convert(Function, Arch::X86, BinaryFormat::COFF);
      EXPECT_TRUE(Med.Params.empty());
      const auto Exits = getRegistrationSEHParentExits(Med);
      ASSERT_TRUE(Exits);
      ASSERT_EQ(Exits->size(), 1u);
      EXPECT_EQ(Exits->begin()->second.Invocations.size(), 2u);
      for (const auto &Block : Med.Blocks)
        for (const auto &Op : Block.Ops)
          if (Op.Opcode == NdOp::CALL &&
              Op.Addr == State.SEHInvocationExits[1].CallVA) {
            EXPECT_TRUE(Op.DoesNotReturn);
            EXPECT_TRUE(Block.Succs.empty());
          }
    }
}

TEST(RegistrationSEHEpilogue, RequiresImmutableCompleteInstructionBoundaries) {
  for (bool Scratch : {false, true})
    for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
      SCOPED_TRACE(Scratch);
      SCOPED_TRACE(Mutation);
      auto Image = epilogueImage(Scratch, 0);
      Decoder Decode;
      ASSERT_TRUE(Decode.init(Image));
      auto Function = CFGBuilder().build(Image, Decode, 0x401000);
      const va_t Start = Image.Segments.front().VA +
                         Image.Segments.front().Size - (Scratch ? 8 : 7);
      auto Epilogue = llvm::find_if(Function.Blocks, [&](const auto &Block) {
        return Block.StartAddr == Start;
      });
      ASSERT_NE(Epilogue, Function.Blocks.end());
      switch (Mutation) {
      case 1:
        Image.Segments.front().Flags =
            Image.Segments.front().Flags | SegmentFlags::Writable;
        break;
      case 2:
        --Image.Segments.front().FileSz;
        break;
      case 3:
        Image.Segments.push_back(Image.Segments.front());
        break;
      case 4:
        Image.Segments.front().Data.back() = 0xcb;
        break;
      case 5:
        Image.Segments.front().Data[Start - 0x401000] = 0x58;
        break;
      case 6:
        Epilogue->InstructionBoundaries.front().TargetMode =
            LowInstructionTargetMode::Thumb;
        break;
      case 7:
        Epilogue->InstructionBoundaries.front().ControlFlags =
            LowInstructionControlFlag::Conditional;
        break;
      case 8:
        Epilogue->InstructionBoundaries.back().Immediate = 4;
        break;
      case 9:
        ++Epilogue->InstructionBoundaries.front().Size;
        break;
      }
      RegistrationCallCalleeIndex Index(Image);
      const auto Controls = Index.sehControlContracts(Function);
      ASSERT_TRUE(Controls);
      EXPECT_EQ(
          llvm::any_of(
              *Controls,
              [](const auto &Control) {
                return Control.Control ==
                       RegistrationSEHControlContract::Kind::ParentEpilogue;
              }),
          Mutation == 0);
    }
}

TEST(RegistrationSEHEpilogue, EpilogueAloneDoesNotRequestCallerState) {
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Image = epilogueImage(false, 0);
    Decoder Decode;
    ASSERT_TRUE(Decode.init(Image));
    auto Function = CFGBuilder().build(Image, Decode, 0x401000);
    auto &Scope = Function.ExceptionMetadata->Registration->Scopes.front();
    const va_t Call = Scope.HandlerVA - 5;
    if (Mutation == 1)
      Scope.IsFinally = false;
    if (Mutation == 2)
      ++Scope.HandlerVA;
    if (Mutation == 3)
      ++Image.Segments.front().Data[Call + 1 - Image.Segments.front().VA];
    const auto Epilogue =
        llvm::find_if(Function.Blocks, [&](const auto &Block) {
          return getRegistrationSEHParentEpilogue(Image, Block).has_value();
        });
    ASSERT_NE(Epilogue, Function.Blocks.end());
    RegistrationCallCalleeIndex Index(Image);
    const auto Controls = Index.sehControlContracts(Function);
    ASSERT_TRUE(Controls);
    EXPECT_EQ(llvm::any_of(
                  *Controls,
                  [](const auto &Control) {
                    return Control.Control ==
                           RegistrationSEHControlContract::Kind::ParentEpilogue;
                  }),
              Mutation == 0);
  }
}

TEST(RegistrationSEHEpilogue, AdjacentCallRequiresCheckedFinallyOwnership) {
  for (bool EH4 : {false, true})
    for (unsigned Mutation = 0; Mutation != 15; ++Mutation) {
      SCOPED_TRACE(EH4);
      SCOPED_TRACE(Mutation);
      auto Image = epilogueImage(false, 0);
      auto &EH = Image.ExceptionMetadata.Functions.front();
      if (EH4) {
        EH.Encoding = ExceptionEncoding::X86ScopeTableEH4;
        EH.Personality = ExceptionPersonality::ExceptHandler4;
      }
      const va_t Address = EH.Registration->Scopes.front().HandlerVA - 5;
      const size_t Offset = Address - Image.Segments.front().VA;
      Decoder Decode;
      ASSERT_TRUE(Decode.init(Image));
      DecodedInsn Insn{};
      ASSERT_EQ(
          Decode.decodeOneForLift(Image.Segments.front().Data.data() + Offset,
                                  5, Address, Insn),
          5);
      std::vector<LowOp> Ops;
      Decode.liftToLow(Insn, Ops);
      ASSERT_EQ(Ops.size(), 2u);
      ASSERT_EQ(Ops.back().Opcode, NdOp::STORE);
      switch (Mutation) {
      case 1:
        EH.Registration->Scopes.clear();
        break;
      case 2:
        EH.Registration->Scopes.front().IsFinally = false;
        break;
      case 3:
        EH.ParseStatus = ExceptionParseStatus::Partial;
        break;
      case 4:
        EH.Cxx.emplace();
        break;
      case 5:
        EH.CodeRange.End = Address + 5;
        break;
      case 6:
        Image.Segments.front().Flags =
            Image.Segments.front().Flags | SegmentFlags::Writable;
        break;
      case 7:
        Image.Segments.front().Data[Offset + 1] = 1;
        break;
      case 8:
        Image.BaseRelocations.push_back({Address + 1, 3});
        break;
      case 9:
        Ops.front().Inputs[1].Offset = 8;
        break;
      case 10:
        Ops.back().Inputs[1].Offset = Address;
        break;
      case 11:
        Ops.back().MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
        break;
      case 12:
        Ops.back().Seq = 0;
        break;
      case 13:
        Ops.push_back(Ops.front());
        break;
      case 14:
        Image.Format = BinaryFormat::ELF;
        break;
      }
      registration_state::restoreAdjacentSEHFinallyCall(Image, EH, Address, 5,
                                                        Ops);
      if (!Mutation) {
        ASSERT_EQ(Ops.size(), 1u);
        EXPECT_EQ(Ops.front().Opcode, NdOp::CALL);
        EXPECT_EQ(Ops.front().Inputs[0], NdVar::cst(Address + 5, 4));
      } else {
        ASSERT_GE(Ops.size(), 2u);
        EXPECT_EQ(Ops.front().Opcode, NdOp::INT_SUB);
        EXPECT_EQ(Ops[1].Opcode, NdOp::STORE);
      }
    }
}
