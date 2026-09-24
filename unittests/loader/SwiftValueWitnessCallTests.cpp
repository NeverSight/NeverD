#include "gtest/gtest.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/Swift/SwiftValueWitnessCalls.h"

using namespace neverd;

namespace {
struct Fixture {
  BinaryImage Image;
  LowFunc Function;
  uint64_t Metadata = 0;
  uint64_t Target = 0;

  explicit Fixture(Arch Architecture, bool Split = false,
                   SourceCallTypeHint::SwiftValueWitnessKind Operation =
                       SourceCallTypeHint::SwiftValueWitnessKind::Destroy) {
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Architecture;
    Image.Bits = Bitness::Bits64;
    const auto Hint = swiftValueWitnessSourceCallHint(Architecture, Operation);
    if (!Hint) {
      ADD_FAILURE() << "missing value-witness ABI";
      return;
    }
    Metadata = Hint->Signature.Parameters.back().Location.RegisterOffset;
    const auto &TRI = getTargetRegInfo(Architecture);
    if (TRI.CalleeSaveRegs.empty()) {
      ADD_FAILURE() << "missing callee-save register";
      return;
    }
    Target = TRI.CalleeSaveRegs.front();
    Function.Entry = 0x1000;
    Function.Blocks.resize(Split ? 2 : 1);
    Function.Blocks[0].Id = 0;
    Function.Blocks[0].StartAddr = Function.Entry;
    if (Split) {
      Function.Blocks[0].Succs = {1};
      Function.Blocks[1].Id = 1;
      Function.Blocks[1].StartAddr = 0x1010;
      Function.Blocks[1].Preds = {0};
    }
    auto Add = [&](unsigned Block, va_t Address, NdOp Code, NdVar Output,
                   std::initializer_list<NdVar> Inputs) {
      LowOp Operation;
      Operation.Addr = Address;
      Operation.Opcode = Code;
      Operation.Output = Output;
      for (const auto &Input : Inputs)
        Operation.addInput(Input);
      Function.Blocks[Block].Ops.push_back(Operation);
    };
    Add(0, 0x1000, NdOp::INT_ADD, NdVar::tmp(TmpBase, 8),
        {NdVar::reg(Metadata, 8), NdVar::cst(UINT64_C(-8), 8)});
    Add(0, 0x1000, NdOp::LOAD, NdVar::tmp(TmpBase + 8, 8),
        {NdVar::tmp(TmpBase, 8)});
    Add(0, 0x1000, NdOp::COPY, NdVar::reg(Target, 8),
        {NdVar::tmp(TmpBase + 8, 8)});
    const unsigned Tail = Split ? 1 : 0;
    const auto Slot = swiftValueWitnessSlot(Operation);
    if (!Slot) {
      ADD_FAILURE() << "missing value-witness slot";
      return;
    }
    Add(Tail, 0x1010, NdOp::INT_ADD, NdVar::tmp(TmpBase, 8),
        {NdVar::reg(Target, 8), NdVar::cst(*Slot * 8, 8)});
    Add(Tail, 0x1010, NdOp::LOAD, NdVar::tmp(TmpBase + 8, 8),
        {NdVar::tmp(TmpBase, 8)});
    Add(Tail, 0x1010, NdOp::COPY, NdVar::reg(Target, 8),
        {NdVar::tmp(TmpBase + 8, 8)});
    Add(Tail, 0x1014, NdOp::INDIR_CALL, {}, {NdVar::reg(Target, 8)});
    Add(Tail, 0x1018, NdOp::RETURN, {}, {});
  }

  std::map<va_t, SourceCallTypeHint> hints() const {
    return buildSwiftValueWitnessCallHints(Image, Function);
  }
};
} // namespace

TEST(SwiftValueWitnessCalls, OperationsUseExactMetadataAndRequiredTableSlots) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (const auto Operation :
         {SourceCallTypeHint::SwiftValueWitnessKind::Destroy,
          SourceCallTypeHint::SwiftValueWitnessKind::InitializeWithCopy,
          SourceCallTypeHint::SwiftValueWitnessKind::
              InitializeBufferWithCopyOfBuffer,
          SourceCallTypeHint::SwiftValueWitnessKind::AssignWithCopy,
          SourceCallTypeHint::SwiftValueWitnessKind::InitializeWithTake,
          SourceCallTypeHint::SwiftValueWitnessKind::AssignWithTake,
          SourceCallTypeHint::SwiftValueWitnessKind::GetEnumTagSinglePayload,
          SourceCallTypeHint::SwiftValueWitnessKind::
              StoreEnumTagSinglePayload}) {
      for (const bool Split : {false, true}) {
        SCOPED_TRACE(static_cast<unsigned>(Architecture));
        SCOPED_TRACE(static_cast<unsigned>(Operation));
        SCOPED_TRACE(Split);
        Fixture F(Architecture, Split, Operation);
        const auto Hints = F.hints();
        ASSERT_EQ(Hints.size(), 1U);
        EXPECT_EQ(Hints.begin()->first, 0x1014U);
        EXPECT_TRUE(isSwiftValueWitnessSourceCallHint(Hints.begin()->second,
                                                      Architecture));
        EXPECT_EQ(Hints.begin()->second.ValueWitness, Operation);

        LowToMedConverter Converter;
        Converter.setBinaryImage(&F.Image);
        Converter.setSourceCallHintsEnabled(true);
        const auto Med =
            Converter.convert(F.Function, Architecture, BinaryFormat::MachO);
        const MedOp *Call = nullptr;
        for (const auto &Block : Med.Blocks)
          for (const auto &Candidate : Block.Ops)
            if (Candidate.SourceCallHint &&
                Candidate.SourceCallHint->CallKind ==
                    SourceCallTypeHint::Kind::SwiftValueWitness)
              Call = &Candidate;
        ASSERT_NE(Call, nullptr);
        EXPECT_EQ(Call->Opcode, NdOp::INDIR_CALL);
        EXPECT_EQ(Call->NumInputs,
                  Hints.begin()->second.Signature.Parameters.size() + 1);
        EXPECT_EQ(Call->Output.Size,
                  Hints.begin()->second.Signature.ReturnType->Size);
      }
    }
  }
}

TEST(SwiftValueWitnessCalls, ImageMetadataPreservesExactWitnessProvenance) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (const bool Fragment : {false, true}) {
      SCOPED_TRACE(static_cast<unsigned>(Architecture));
      SCOPED_TRACE(Fragment);
      Fixture F(Architecture, false,
                SourceCallTypeHint::SwiftValueWitnessKind::InitializeWithCopy);
      Segment Data;
      Data.VA = 0x4000;
      Data.Size = Data.FileSz = 0x1000;
      Data.Flags = SegmentFlags::Readable;
      Data.Data.resize(0x1000);
      F.Image.Segments.push_back(Data);
      Section Storage;
      Storage.Name = "__const";
      Storage.VA = Data.VA;
      Storage.Size = Storage.FileSz = Data.Size;
      Storage.Flags = Data.Flags;
      F.Image.Sections.push_back(Storage);

      auto SetMetadata = [&](NdVar Value) {
        LowOp Definition;
        Definition.Addr = F.Function.Entry;
        Definition.Opcode = Fragment ? NdOp::INT_ADD : NdOp::COPY;
        Definition.Output = NdVar::reg(F.Metadata, 8);
        Definition.addInput(Value);
        if (Fragment)
          Definition.addInput(NdVar::scalar(0x800, 8));
        F.Function.Blocks[0].Ops.insert(F.Function.Blocks[0].Ops.begin(),
                                        Definition);
      };
      SetMetadata(Fragment ? NdVar::addressFragment(0x4000, 8)
                           : NdVar::dataAddress(0x4800, 8));
      EXPECT_EQ(F.hints().size(), 1U);

      auto Literal = [&]() -> NdVar & {
        return F.Function.Blocks[0].Ops.front().Inputs[0];
      };
      if (!Fragment) {
        Literal().AddressOwnerVA = 0x4000;
        LowOp Replacement;
        Replacement.Addr = 0x1014;
        Replacement.Opcode = NdOp::COPY;
        Replacement.Output = NdVar::reg(F.Metadata, 8);
        Replacement.addInput(NdVar::dataAddress(0x4800, 8, 0x4010));
        auto &Ops = F.Function.Blocks[0].Ops;
        Ops.insert(Ops.end() - 2, Replacement);
        EXPECT_TRUE(F.hints().empty());
        Ops.erase(Ops.end() - 3);
        Literal().AddressOwnerVA = InvalidVA;
      }
      Literal().Provenance = ConstantAddressProvenance::Scalar;
      EXPECT_TRUE(F.hints().empty());
      Literal().Provenance = Fragment
                                 ? ConstantAddressProvenance::AddressFragment
                                 : ConstantAddressProvenance::DataAddress;
      Literal().Offset = Fragment ? 0x5000 : 0x5800;
      EXPECT_TRUE(F.hints().empty());
      Literal().Offset = Fragment ? 0x4000 : 0x4800;
      F.Image.Segments.front().Data.resize(0x7f8);
      EXPECT_TRUE(F.hints().empty());
    }
  }
}

TEST(SwiftValueWitnessCalls, DestroyProofRejectsNearMatchesAndAmbiguity) {
  for (unsigned Case = 0; Case < 10; ++Case) {
    SCOPED_TRACE(Case);
    Fixture F(Arch::AArch64);
    auto &Operations = F.Function.Blocks.front().Ops;
    switch (Case) {
    case 0:
      Operations[0].Inputs[1].Offset = UINT64_C(-16);
      break;
    case 1:
      Operations[3].Inputs[1].Offset = 16;
      break;
    case 2:
      Operations[1].MemoryOrdering = NdMemoryOrdering::Acquire;
      break;
    case 3:
      Operations[4].MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      break;
    case 4: {
      LowOp Change;
      Change.Addr = 0x1014;
      Change.Opcode = NdOp::COPY;
      Change.Output = NdVar::reg(F.Metadata, 8);
      Change.addInput(NdVar::reg(F.Target, 8));
      Operations.insert(Operations.end() - 2, Change);
      break;
    }
    case 5: {
      LowOp Partial;
      Partial.Addr = 0x1014;
      Partial.Opcode = NdOp::COPY;
      Partial.Output = NdVar::reg(F.Target, 4);
      Partial.addInput(NdVar::cst(0, 4));
      Operations.insert(Operations.end() - 2, Partial);
      break;
    }
    case 6:
      Operations[6].Opcode = NdOp::CALL;
      break;
    case 7: {
      LowOp Duplicate = Operations[6];
      Operations.insert(Operations.end() - 1, Duplicate);
      break;
    }
    case 8:
      Operations[0].MemoryOrdering = NdMemoryOrdering::Acquire;
      break;
    case 9:
      Operations[2].MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      break;
    }
    EXPECT_TRUE(F.hints().empty());
  }
}

TEST(SwiftValueWitnessCalls, DestroyProofRejectsConflictingJoinPaths) {
  Fixture F(Arch::AArch64, true);
  LowBlock Other = F.Function.Blocks.front();
  Other.Id = 2;
  Other.StartAddr = 0x1080;
  Other.Succs = {1};
  Other.Ops[0].Inputs[1].Offset = UINT64_C(-16);
  F.Function.Blocks[1].Preds.push_back(2);
  F.Function.Blocks.push_back(std::move(Other));
  EXPECT_TRUE(F.hints().empty());
}

TEST(SwiftValueWitnessCalls, DestroyProofRejectsMalformedControlFlow) {
  for (unsigned Case = 0; Case < 5; ++Case) {
    SCOPED_TRACE(Case);
    Fixture F(Arch::AArch64, true);
    switch (Case) {
    case 0:
      F.Function.Blocks[0].Succs.clear();
      break;
    case 1:
      F.Function.Blocks[1].Preds.clear();
      break;
    case 2:
      F.Function.Blocks[1].Id = F.Function.Blocks[0].Id;
      break;
    case 3:
      F.Function.Blocks[1].Preds.push_back(0);
      break;
    case 4: {
      LowBlock Hidden;
      Hidden.Id = 2;
      Hidden.StartAddr = 0x1080;
      Hidden.Succs = {1};
      F.Function.Blocks.push_back(std::move(Hidden));
      break;
    }
    }
    EXPECT_TRUE(F.hints().empty());
  }
}
