//===- ModuleAdjacentTableTests.cpp - Exact foreign table sources --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checks adjacent absolute tables without granting arbitrary one-past uses.
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"
#include "PipelineLowIRDetail.h"

#include "neverd/Limits.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/SourceUnwind.h"
#include "neverd/support/BinaryLoading.h"

#include "llvm/Support/Error.h"

#include <algorithm>
#include <fstream>
#include <optional>
#include <set>

using namespace neverd;

namespace {

class ModuleAdjacentTable : public NeverDLiftTest {
protected:
  struct Inputs {
    BinaryImage Image;
    std::vector<LowFunc> Functions;
    fs::path Object;
  };

  void SetUp() override {
    NeverDLiftTest::SetUp();
    if (!hasCrossTargetClang())
      GTEST_SKIP() << "cross-target Clang is unavailable";
  }

  std::optional<Inputs> fixture(bool X64, unsigned FloatMode = 0,
                                bool ThreeTables = false, bool CFI = false,
                                unsigned DamagedCFI = 0) {
    const auto Source =
        fs::path(TEST_SOURCE_DIR) / "x86_64/test_module_adjacent_absolute.S";
    const auto Object = tmpFile(X64 ? "adjacent64.o" : "adjacent32.o");
    const auto Built = exec(
        NEVERD_TEST_CLANG, {"-target",
                            CFI ? "i686-windows-gnu"
                                : (X64 ? "x86_64-linux-gnu" : "i386-linux-gnu"),
                            "-DFLOAT_MODE=" + std::to_string(FloatMode),
                            "-DTHREE_TABLES=" + std::to_string(ThreeTables),
                            "-c", "-DCFI_MODE=" + std::to_string(CFI),
                            "-DDAMAGED_CFI=" + std::to_string(DamagedCFI),
                            Source.string(), "-o", Object.string()});
    EXPECT_TRUE(Built.ok()) << Built.err;
    if (!Built.ok())
      return std::nullopt;
    auto Input = Object;
    if (CFI) {
      // Real PE32 CFI, including base-relocated table fields.
      EXPECT_FALSE(X64);
      Input = tmpFile("adjacent-cfi32.exe");
      std::vector<std::string> LinkArgs = {"--entry",
                                           "_adjacent_first",
                                           "--subsystem",
                                           "console",
                                           "--image-base",
                                           "0x400000",
                                           "--export-all-symbols",
                                           "--enable-long-section-names",
                                           Object.string(),
                                           "-o",
                                           Input.string()};
      if (DamagedCFI == 2 || DamagedCFI == 4) {
        LinkArgs.push_back("--defsym");
        LinkArgs.push_back("adjacent_missing_lsda=0x12345678");
      }
      const auto Linked = exec("i686-w64-mingw32-ld", LinkArgs);
      EXPECT_TRUE(Linked.ok()) << Linked.err;
      if (!Linked.ok())
        return std::nullopt;
      if (DamagedCFI == 1 || DamagedCFI == 3) {
        std::ifstream In(Input, std::ios::binary);
        std::string Bytes((std::istreambuf_iterator<char>(In)), {});
        const auto Augmentation = Bytes.find(std::string("zRS\0", 4));
        EXPECT_NE(Augmentation, std::string::npos);
        if (Augmentation == std::string::npos)
          return std::nullopt;
        EXPECT_EQ(Bytes.find(std::string("zRS\0", 4), Augmentation + 4),
                  std::string::npos);
        // Keep the recognized R encoding before the unknown character, so
        // the real loader still reads the FDE's function address correctly.
        Bytes[Augmentation + 2] = 'X';
        std::ofstream Out(Input, std::ios::binary | std::ios::trunc);
        Out.write(Bytes.data(), Bytes.size());
      }
    }
    auto Image = loadBinary(Input);
    EXPECT_TRUE(static_cast<bool>(Image));
    if (!Image) {
      llvm::consumeError(Image.takeError());
      return std::nullopt;
    }
    Inputs Result{std::move(*Image), {}, Input};
    std::vector<const char *> Names{"adjacent_first", "adjacent_second"};
    if (ThreeTables)
      Names.push_back("adjacent_third");
    std::set<va_t> Entries;
    for (const char *Name : Names) {
      const Symbol *S = Result.Image.findSymbol(Name);
      if (!S && CFI)
        S = Result.Image.findSymbol(std::string("_") + Name);
      EXPECT_NE(S, nullptr);
      if (!S)
        return std::nullopt;
      Entries.insert(S->Addr);
    }
    Decoder Dec;
    EXPECT_TRUE(Dec.init(Result.Image.Arch));
    CFGBuilder Builder;
    Builder.setKnownFuncEntries(&Entries);
    for (const char *Name : Names) {
      const Symbol *S = Result.Image.findSymbol(Name);
      if (!S && CFI)
        S = Result.Image.findSymbol(std::string("_") + Name);
      Result.Functions.push_back(
          Builder.build(Result.Image, Dec, S->Addr, Name));
    }
    return Result;
  }

  static std::vector<va_t> firstSlots(const Inputs &I) {
    for (const LowFunc &F : I.Functions)
      if (F.Name == "adjacent_first" && F.JumpTables.size() == 1)
        return F.JumpTables.front().SuppressibleRelocationSlots;
    return {};
  }

  static void expectFirstProtected(const Inputs &I) {
    const auto Slots = firstSlots(I);
    ASSERT_EQ(Slots.size(), 2u);
    const auto Result = pipeline_detail::arbitrateModuleJumpTablesForTesting(
        I.Image, I.Functions);
    ASSERT_TRUE(Result.AnalysisComplete);
    for (va_t Slot : Slots)
      EXPECT_EQ(Result.ProtectedRelocationSlots.count(Slot), 1u);
  }

  static void expectFirstUnprotected(const Inputs &I) {
    const auto Slots = firstSlots(I);
    ASSERT_EQ(Slots.size(), 2u);
    const auto Result = pipeline_detail::arbitrateModuleJumpTablesForTesting(
        I.Image, I.Functions);
    ASSERT_TRUE(Result.AnalysisComplete);
    for (va_t Slot : Slots)
      EXPECT_EQ(Result.ProtectedRelocationSlots.count(Slot), 0u);
  }

  static const RelocatedInstructionAddressOccurrence *source(const LowFunc &F) {
    if (F.JumpTables.size() != 1)
      return nullptr;
    for (const auto &S : F.RelocatedInstructionAddressOccurrences)
      if (S.InstructionAddr == F.JumpTables.front().InsnAddr)
        return &S;
    return nullptr;
  }

  static LowOp *load(LowFunc &F) {
    if (F.JumpTables.size() != 1)
      return nullptr;
    for (auto &B : F.Blocks)
      for (auto &Op : B.Ops)
        if (Op.Addr == F.JumpTables.front().InsnAddr && Op.Opcode == NdOp::LOAD)
          return &Op;
    return nullptr;
  }

  static bool exposeAddress(LowFunc &F, bool AlterProvenance) {
    const auto *L = load(F);
    if (!L || !L->NumInputs)
      return false;
    NdVar Address = L->Inputs[0];
    if (AlterProvenance) {
      Address.Provenance = ConstantAddressProvenance::Scalar;
      Address.AddressOwnerVA = 7;
    }
    const va_t Dispatch = F.JumpTables.front().InsnAddr;
    for (auto &B : F.Blocks)
      for (size_t I = 0; I < B.InstructionBoundaries.size(); ++I) {
        auto &Boundary = B.InstructionBoundaries[I];
        if (Boundary.Address != Dispatch || Boundary.OpCount == 0)
          continue;
        int MaxSeq = -1;
        for (const LowOp &Op : B.Ops)
          if (Op.Addr == Dispatch)
            MaxSeq = std::max(MaxSeq, Op.Seq);
        LowOp Copy;
        Copy.Addr = Dispatch;
        Copy.Seq = MaxSeq + 1;
        Copy.Opcode = NdOp::COPY;
        Copy.Output = NdVar::reg(x86reg::RAX, 4);
        Copy.Inputs[0] = Address;
        Copy.NumInputs = 1;
        B.Ops.insert(B.Ops.begin() + Boundary.FirstOp + Boundary.OpCount - 1,
                     Copy);
        ++Boundary.OpCount;
        for (size_t J = I + 1; J < B.InstructionBoundaries.size(); ++J)
          ++B.InstructionBoundaries[J].FirstOp;
        return true;
      }
    return false;
  }

  static bool splitDispatch(LowFunc &F, bool IndependentRoot) {
    const va_t Dispatch = F.JumpTables.front().InsnAddr;
    for (size_t BI = 0; BI < F.Blocks.size(); ++BI) {
      const auto &B = F.Blocks[BI];
      for (size_t I = 1; I < B.InstructionBoundaries.size(); ++I) {
        if (B.InstructionBoundaries[I].Address != Dispatch)
          continue;
        const int TailId = static_cast<int>(BI + 1);
        const auto Remap = [&](int Id) { return Id >= TailId ? Id + 1 : Id; };
        for (auto &Block : F.Blocks) {
          Block.Id = Remap(Block.Id);
          for (int &Id : Block.Preds)
            Id = Remap(Id);
          for (int &Id : Block.Succs)
            Id = Remap(Id);
        }
        auto &Head = F.Blocks[BI];
        LowBlock Tail;
        Tail.Id = TailId;
        Tail.StartAddr = Dispatch;
        Tail.EndAddr = Head.EndAddr;
        Tail.Succs = Head.Succs;
        Tail.Preds = {Head.Id};
        const size_t FirstOp = Head.InstructionBoundaries[I].FirstOp;
        Tail.Ops.assign(Head.Ops.begin() + FirstOp, Head.Ops.end());
        Tail.InstructionBoundaries.assign(Head.InstructionBoundaries.begin() +
                                              I,
                                          Head.InstructionBoundaries.end());
        for (auto &Boundary : Tail.InstructionBoundaries)
          Boundary.FirstOp -= FirstOp;
        for (int Succ : Tail.Succs) {
          auto &Target = F.Blocks[Succ > TailId ? Succ - 1 : Succ];
          for (int &Pred : Target.Preds)
            if (Pred == Head.Id)
              Pred = TailId;
        }
        Head.EndAddr = Dispatch;
        Head.Succs = {TailId};
        Head.Ops.resize(FirstOp);
        Head.InstructionBoundaries.resize(I);
        F.Blocks.insert(F.Blocks.begin() + BI + 1, std::move(Tail));
        if (IndependentRoot) {
          F.ModuleAnalysisRoots.insert(Dispatch);
          F.OrdinaryModuleAnalysisRoots.insert(Dispatch);
        }
        return true;
      }
    }
    return false;
  }

  static LowFunc sourceOnly(const LowFunc &F) {
    LowFunc Result;
    const auto *S = source(F);
    if (!S)
      return Result;
    Result.Entry = S->InstructionAddr;
    Result.Name = "independent_source_only_context";
    Result.DecodedInstructionCount = Result.LiftedInstructionCount = 1;
    Result.ModuleAnalysisRoots =
        Result.OrdinaryModuleAnalysisRoots = {Result.Entry};
    Result.RelocatedInstructionAddressOccurrences = {*S};
    LowBlock Block;
    Block.Id = 0;
    Block.StartAddr = Result.Entry;
    for (const auto &B : F.Blocks)
      for (const auto &Boundary : B.InstructionBoundaries) {
        if (Boundary.Address != Result.Entry)
          continue;
        for (size_t I = Boundary.FirstOp;
             I < Boundary.FirstOp + Boundary.OpCount; ++I) {
          if (B.Ops[I].Opcode == NdOp::LOAD)
            break;
          Block.Ops.push_back(B.Ops[I]);
        }
        LowOp Return;
        Return.Addr = Result.Entry;
        Return.Seq = Block.Ops.empty() ? 0 : Block.Ops.back().Seq + 1;
        Return.Opcode = NdOp::RETURN;
        Return.NumInputs = 1;
        Return.Inputs[0] = NdVar::scalar(0, 4);
        Block.Ops.push_back(Return);
        auto Published = Boundary;
        Published.FirstOp = 0;
        Published.OpCount = Block.Ops.size();
        Published.Control = LowInstructionControl::Return;
        Published.ControlFlags = LowInstructionControlFlag::Return;
        Block.EndAddr = Result.Entry + Published.Size;
        Block.InstructionBoundaries.push_back(Published);
      }
    Result.Blocks.push_back(std::move(Block));
    return Result;
  }
};

TEST_F(ModuleAdjacentTable,
       IndependentBoundedDispatchDoesNotExposePriorObject) {
  for (bool X64 : {false, true}) {
    SCOPED_TRACE(X64);
    auto I = fixture(X64);
    ASSERT_TRUE(I);
    ASSERT_EQ(I->Functions.size(), 2u);
    for (const LowFunc &F : I->Functions) {
      ASSERT_TRUE(F.hasCompleteInstructionLift());
      ASSERT_EQ(F.JumpTables.size(), 1u);
      ASSERT_EQ(F.JumpTables.front().SuppressibleRelocationSlots.size(), 2u);
      ASSERT_EQ(F.JumpTables.front().AuthenticatedTableLoads.size(), 1u);
    }
    const auto End =
        I->Functions[0].JumpTables.front().StorageRanges.front().storageEnd();
    ASSERT_TRUE(End);
    ASSERT_EQ(*End, I->Functions[1].JumpTables.front().BaseAddr);
    const auto Slots = firstSlots(*I);
    ASSERT_FALSE(Slots.empty());
    for (unsigned Order : {0u, 1u}) {
      SCOPED_TRACE(Order);
      const auto Result = pipeline_detail::arbitrateModuleJumpTablesForTesting(
          I->Image, I->Functions);
      ASSERT_TRUE(Result.AnalysisComplete);
      for (va_t Slot : Slots)
        EXPECT_EQ(Result.ProtectedRelocationSlots.count(Slot), 0u);
      std::reverse(I->Functions.begin(), I->Functions.end());
    }
  }
}

TEST_F(ModuleAdjacentTable,
       LoaderFieldWithoutPublishedSourceRetainsPriorObject) {
  for (bool X64 : {false, true}) {
    SCOPED_TRACE(X64);
    auto I = fixture(X64);
    ASSERT_TRUE(I);
    ASSERT_EQ(I->Functions.size(), 2u);
    ASSERT_FALSE(
        I->Functions.back().RelocatedInstructionAddressOccurrences.empty());
    I->Functions.back().RelocatedInstructionAddressOccurrences.clear();
    expectFirstProtected(*I);
    std::reverse(I->Functions.begin(), I->Functions.end());
    expectFirstProtected(*I);
  }
}

TEST_F(ModuleAdjacentTable, AllOverlappingImageRecordsMustBePlain) {
  for (bool X64 : {false, true})
    for (unsigned Binding : {0u, 1u, 2u}) {
      SCOPED_TRACE(X64);
      SCOPED_TRACE(Binding);
      auto I = fixture(X64);
      ASSERT_TRUE(I);
      auto &F = I->Functions[1];
      ASSERT_TRUE(source(F));
      ExceptionFunction Plain;
      Plain.CodeRange = {F.Entry, F.Blocks.back().EndAddr};
      Plain.Encoding = ExceptionEncoding::DwarfFDE;
      Plain.Dwarf.emplace();
      ASSERT_TRUE(isPlainSourceUnwind(Plain));
      ExceptionFunction Outer = Plain;
      Outer.Personality = ExceptionPersonality::GxxPersonalityV0;
      Outer.PersonalityVA = F.Entry;
      if (Binding == 0)
        Outer.CodeRange = {I->Functions[0].Entry, Plain.CodeRange.End};
      else {
        Outer.CodeRange = {I->Functions[0].Entry,
                           I->Functions[0].Blocks.back().EndAddr};
        if (Binding == 1)
          Outer.FragmentRanges.push_back(Plain.CodeRange);
        else
          Outer.FunctionEntry = F.Entry;
      }
      I->Image.ExceptionMetadata.Functions = {Plain, Outer};
      for (unsigned Order : {0u, 1u}) {
        I->Image.ExceptionMetadata.rebuildIndex();
        for (bool CopyPlain : {false, true}) {
          F.ExceptionMetadata = CopyPlain ? std::optional(Plain) : std::nullopt;
          size_t Work = limits::kMaxJumpTableCandidateEvidenceWork;
          EXPECT_FALSE(CFGBuilder::proveInstructionLocalTableSource(
              I->Image, F, *source(F), Work));
          expectFirstProtected(*I);
        }
        std::reverse(I->Image.ExceptionMetadata.Functions.begin(),
                     I->Image.ExceptionMetadata.Functions.end());
      }
      // An unrelated language record must not disable this local proof.
      Outer.FragmentRanges.clear();
      Outer.FunctionEntry = 0;
      Outer.CodeRange = {I->Functions[0].Entry,
                         I->Functions[0].Blocks.back().EndAddr};
      I->Image.ExceptionMetadata.Functions = {Plain, Outer};
      I->Image.ExceptionMetadata.rebuildIndex();
      F.ExceptionMetadata.reset();
      size_t Work = limits::kMaxJumpTableCandidateEvidenceWork;
      EXPECT_TRUE(CFGBuilder::proveInstructionLocalTableSource(
          I->Image, F, *source(F), Work));
      expectFirstUnprotected(*I);
    }
}

TEST_F(ModuleAdjacentTable, MalformedFrameEntrySurvivesMissingFinalAnnotation) {
  for (bool X64 : {false, true}) {
    auto I = fixture(X64);
    ASSERT_TRUE(I);
    auto &F = I->Functions[1];
    ASSERT_TRUE(source(F));
    ASSERT_TRUE(I->Image.findSymbol(F.Name));
    ASSERT_TRUE(I->Image.findSymbol(F.Name)->IsFunc);
    ExceptionFunction Malformed;
    Malformed.Encoding = ExceptionEncoding::DwarfFDE;
    Malformed.ParseStatus = ExceptionParseStatus::Malformed;
    Malformed.Dwarf.emplace();
    Malformed.Dwarf->InitialLocation = F.Entry;
    I->Image.ExceptionMetadata.Functions.push_back(Malformed);
    I->Image.ExceptionMetadata.rebuildIndex();
    F.ExceptionMetadata.reset();
    size_t Work = limits::kMaxJumpTableCandidateEvidenceWork;
    EXPECT_FALSE(CFGBuilder::proveInstructionLocalTableSource(
        I->Image, F, *source(F), Work));
    expectFirstProtected(*I);
    std::reverse(I->Functions.begin(), I->Functions.end());
    expectFirstProtected(*I);
  }
}

TEST_F(ModuleAdjacentTable, SlotInventoryMustPartitionTheFreshRuntimeEnvelope) {
  for (bool X64 : {false, true}) {
    auto I = fixture(X64);
    ASSERT_TRUE(I);
    auto &F = I->Functions[1];
    ASSERT_TRUE(source(F));
    auto &Ranges = F.JumpTables.front().StorageRanges;
    ASSERT_EQ(Ranges.size(), 1u);
    const auto Original = Ranges.front();
    ASSERT_EQ(Original.PhysicalSlotCount, 2u);
    Ranges = {Original, Original};
    Ranges[0].PhysicalSlotCount = Ranges[1].PhysicalSlotCount = 1;
    Ranges[1].BaseAddr += Original.EntrySize;
    for (unsigned Order : {0u, 1u}) {
      size_t Work = limits::kMaxJumpTableCandidateEvidenceWork;
      EXPECT_TRUE(CFGBuilder::proveInstructionLocalTableSource(
          I->Image, F, *source(F), Work));
      expectFirstUnprotected(*I);
      std::reverse(Ranges.begin(), Ranges.end());
    }
    for (unsigned Damage : {0u, 1u, 2u, 3u}) {
      SCOPED_TRACE(Damage);
      const auto Saved = Ranges;
      if (Damage == 0)
        Ranges[1].BaseAddr = Ranges[0].BaseAddr;
      else if (Damage == 1)
        Ranges[1].BaseAddr += Original.EntrySize;
      else if (Damage == 2)
        Ranges[0].EntryStride *= 2;
      else
        Ranges.pop_back();
      size_t Work = limits::kMaxJumpTableCandidateEvidenceWork;
      EXPECT_FALSE(CFGBuilder::proveInstructionLocalTableSource(
          I->Image, F, *source(F), Work));
      expectFirstProtected(*I);
      Ranges = Saved;
    }
  }
}

TEST_F(ModuleAdjacentTable, PlainUnwindDoesNotIntroduceExceptionDispatch) {
  if (!exec("i686-w64-mingw32-ld", {"--version"}).ok())
    GTEST_SKIP() << "the real PE32 CFI fixture needs the MinGW linker";
  for (bool X64 : {false}) {
    auto I = fixture(X64, 1, false, true);
    ASSERT_TRUE(I);
    const auto &F = I->Functions[1];
    ASSERT_TRUE(F.ExceptionMetadata);
    ASSERT_EQ(F.ExceptionMetadata->Encoding, ExceptionEncoding::DwarfFDE);
    ASSERT_TRUE(F.ExceptionMetadata->Dwarf);
    ASSERT_EQ(F.ExceptionMetadata->Dwarf->LSDAVA, 0u);
    ASSERT_TRUE(source(F));
    size_t Work = limits::kMaxJumpTableCandidateEvidenceWork;
    EXPECT_TRUE(CFGBuilder::proveInstructionLocalTableSource(I->Image, F,
                                                             *source(F), Work));
    expectFirstUnprotected(*I);
    std::reverse(I->Functions.begin(), I->Functions.end());
    expectFirstUnprotected(*I);
  }
}

TEST_F(ModuleAdjacentTable, IncompleteFrameSectionCannotLookLikePlainUnwind) {
  if (!exec("i686-w64-mingw32-ld", {"--version"}).ok())
    GTEST_SKIP() << "the real PE32 CFI fixture needs the MinGW linker";
  for (bool X64 : {false})
    for (unsigned Damage : {1u, 2u, 3u, 4u}) {
      auto I = fixture(X64, 1, false, true, Damage);
      ASSERT_TRUE(I);
      ASSERT_EQ(I->Image.ExceptionMetadata.ParseStatus,
                ExceptionParseStatus::Partial);
      const auto &F = I->Functions[1];
      ASSERT_TRUE(F.ExceptionMetadata);
      ASSERT_TRUE(F.ExceptionMetadata->Dwarf);
      ASSERT_EQ(F.ExceptionMetadata->Dwarf->LSDAVA, 0u);
      EXPECT_EQ(F.ExceptionMetadata->ParseStatus,
                ExceptionParseStatus::Partial);
      ASSERT_TRUE(source(F));
      size_t Work = limits::kMaxJumpTableCandidateEvidenceWork;
      EXPECT_FALSE(CFGBuilder::proveInstructionLocalTableSource(
          I->Image, F, *source(F), Work));
      EXPECT_FALSE(isPlainSourceUnwind(*F.ExceptionMetadata));
      ASSERT_TRUE(F.ExceptionMetadata->DecodeProvenance);
      EXPECT_EQ(F.ExceptionMetadata->DecodeProvenance->Structural.ParseStatus,
                ExceptionParseStatus::Partial);
      // One bad CIE/FDE conservatively downgrades every frame in this section.
      for (const auto &Metadata : I->Image.ExceptionMetadata.Functions)
        EXPECT_EQ(Metadata.ParseStatus, ExceptionParseStatus::Partial);
    }
}

TEST_F(ModuleAdjacentTable, ReplaysFinalFloatingReturnInsteadOfRawReturn) {
  for (bool X64 : {false, true}) {
    auto I = fixture(X64, 1);
    ASSERT_TRUE(I);
    const auto &F = I->Functions[1];
    ASSERT_TRUE(source(F));
    bool SawFloatingReturn = false;
    for (const auto &B : F.Blocks)
      for (const auto &Op : B.Ops)
        if (Op.Opcode == NdOp::RETURN && Op.NumInputs &&
            Op.Inputs[0].Size == 10)
          SawFloatingReturn = true;
    ASSERT_TRUE(SawFloatingReturn);
    size_t Work = limits::kMaxJumpTableCandidateEvidenceWork;
    const auto Access = CFGBuilder::proveInstructionLocalTableSource(
        I->Image, F, *source(F), Work);
    ASSERT_TRUE(Access);
    EXPECT_EQ(Access->BaseAddr, F.JumpTables.front().BaseAddr);
    for (unsigned Order : {0u, 1u}) {
      const auto Result = pipeline_detail::arbitrateModuleJumpTablesForTesting(
          I->Image, I->Functions);
      ASSERT_TRUE(Result.AnalysisComplete);
      for (va_t Slot : firstSlots(*I))
        EXPECT_EQ(Result.ProtectedRelocationSlots.count(Slot), 0u);
      std::reverse(I->Functions.begin(), I->Functions.end());
    }
  }
}

TEST_F(ModuleAdjacentTable, ChangedLoadAddressOrWidthRevokesSourcePermission) {
  for (bool X64 : {false, true})
    for (bool Width : {false, true}) {
      auto I = fixture(X64);
      ASSERT_TRUE(I);
      auto *L = load(I->Functions[1]);
      ASSERT_NE(L, nullptr);
      if (Width)
        L->Output.Size /= 2;
      else
        L->Inputs[0].Offset += 0x2000;
      expectFirstProtected(*I);
    }
}

TEST_F(ModuleAdjacentTable, ExposedAddressAndProvenanceAliasRetainPriorObject) {
  for (bool X64 : {false, true})
    for (bool AlterProvenance : {false, true}) {
      auto I = fixture(X64);
      ASSERT_TRUE(I);
      ASSERT_TRUE(exposeAddress(I->Functions[1], AlterProvenance));
      expectFirstProtected(*I);
      std::reverse(I->Functions.begin(), I->Functions.end());
      expectFirstProtected(*I);
    }
}

TEST_F(ModuleAdjacentTable, SegmentedLoadCannotBorrowDefaultSpaceProof) {
  for (bool X64 : {false, true})
    for (auto Space :
         {NdMemoryAddressSpace::X86FS, NdMemoryAddressSpace::X86GS}) {
      auto I = fixture(X64);
      ASSERT_TRUE(I);
      auto &F = I->Functions[1];
      ASSERT_TRUE(source(F));
      auto *L = load(F);
      ASSERT_NE(L, nullptr);
      L->MemoryAddressSpace = Space;
      size_t Work = limits::kMaxJumpTableCandidateEvidenceWork;
      EXPECT_FALSE(CFGBuilder::proveInstructionLocalTableSource(
          I->Image, F, *source(F), Work));
      expectFirstProtected(*I);
    }
}

TEST_F(ModuleAdjacentTable, UnboundedIndexCannotBorrowPublishedDispatch) {
  for (bool X64 : {false, true}) {
    auto I = fixture(X64);
    ASSERT_TRUE(I);
    bool Changed = false;
    for (auto &B : I->Functions[1].Blocks)
      for (auto &Op : B.Ops)
        if (Op.Opcode == NdOp::INT_AND && Op.Output.Size == 4 &&
            Op.NumInputs == 2 && Op.Inputs[1].isConst() &&
            Op.Inputs[1].Offset == 1) {
          Op.Inputs[1].Offset = UINT32_MAX;
          Changed = true;
        }
    ASSERT_TRUE(Changed);
    expectFirstProtected(*I);
  }
}

TEST_F(ModuleAdjacentTable, DuplicateFieldContextsCannotSharePermission) {
  for (bool X64 : {false, true}) {
    auto I = fixture(X64);
    ASSERT_TRUE(I);
    auto &F = I->Functions[1];
    ASSERT_TRUE(source(F));
    auto Other = *source(F);
    Other.InputIndex = -1;
    F.RelocatedInstructionAddressOccurrences.push_back(Other);
    expectFirstProtected(*I);
    std::reverse(F.RelocatedInstructionAddressOccurrences.begin(),
                 F.RelocatedInstructionAddressOccurrences.end());
    expectFirstProtected(*I);
  }
}

TEST_F(ModuleAdjacentTable, CompletePhysicalEnvelopeIncludesUnusedSlots) {
  for (bool X64 : {false, true}) {
    auto I = fixture(X64);
    ASSERT_TRUE(I);
    auto &Old = I->Functions[0].JumpTables.front();
    ASSERT_EQ(Old.StorageRanges.size(), 1u);
    auto Larger = Old.StorageRanges.front();
    Larger.PhysicalSlotCount = 4;
    Old.ExactPhysicalStorageRange = Larger;
    expectFirstProtected(*I);
  }
}

TEST_F(ModuleAdjacentTable, SharedReplayExhaustionRestoresEarlierGrants) {
  for (bool X64 : {false, true}) {
    auto I = fixture(X64, 0, true);
    ASSERT_TRUE(I);
    ASSERT_EQ(I->Functions.size(), 3u);
    const auto Complete = pipeline_detail::arbitrateModuleJumpTablesForTesting(
        I->Image, I->Functions);
    ASSERT_TRUE(Complete.AnalysisComplete);
    EXPECT_TRUE(Complete.ProtectedRelocationSlots.empty());
    auto &F = I->Functions[1];
    ASSERT_TRUE(source(F));
    size_t Work = limits::kMaxJumpTableCandidateEvidenceWork;
    ASSERT_TRUE(CFGBuilder::proveInstructionLocalTableSource(I->Image, F,
                                                             *source(F), Work));
    const size_t Spent = limits::kMaxJumpTableCandidateEvidenceWork - Work;
    ASSERT_GT(Spent, 4096u);
    Work = Spent + limits::kMaxI386GOTOFFProposalEvidenceWork;
    EXPECT_FALSE(CFGBuilder::proveInstructionLocalTableSource(
        I->Image, F, *source(F), Work));
    EXPECT_EQ(Work, 0u);
    const size_t Limited =
        Spent + limits::kMaxI386GOTOFFProposalEvidenceWork + 1;
    Work = Limited;
    ASSERT_TRUE(CFGBuilder::proveInstructionLocalTableSource(I->Image, F,
                                                             *source(F), Work));
    EXPECT_EQ(Work, limits::kMaxI386GOTOFFProposalEvidenceWork + 1);
    EXPECT_FALSE(CFGBuilder::proveInstructionLocalTableSource(
        I->Image, F, *source(F), Work));
    EXPECT_EQ(Work, 0u);
    const auto Result = pipeline_detail::arbitrateModuleJumpTablesForTesting(
        I->Image, I->Functions, Limited);
    EXPECT_FALSE(Result.AnalysisComplete);
    std::set<va_t> Requested;
    for (const auto &Function : I->Functions)
      for (const auto &Table : Function.JumpTables)
        Requested.insert(Table.SuppressibleRelocationSlots.begin(),
                         Table.SuppressibleRelocationSlots.end());
    ASSERT_EQ(Requested.size(), 6u);
    EXPECT_EQ(Result.ProtectedRelocationSlots, Requested);
  }
}

TEST_F(ModuleAdjacentTable, InteriorBranchCannotReplayBypassedMask) {
  for (bool X64 : {false, true}) {
    auto I = fixture(X64);
    ASSERT_TRUE(I);
    auto &F = I->Functions[1];
    ASSERT_TRUE(source(F));
    size_t Work = limits::kMaxJumpTableCandidateEvidenceWork;
    ASSERT_TRUE(CFGBuilder::proveInstructionLocalTableSource(I->Image, F,
                                                             *source(F), Work));
    const va_t Dispatch = F.JumpTables.front().InsnAddr;
    ASSERT_NE(F.Blocks.front().StartAddr, Dispatch);
    auto &Case = F.Blocks[1];
    ASSERT_TRUE(Case.Succs.empty());
    ASSERT_EQ(Case.Ops.back().Opcode, NdOp::RETURN);
    // This final graph did not split the interior dispatch target. The edge
    // linker must not drop this edge while replay maps it back to the mask.
    Case.Ops.back().Opcode = NdOp::BRANCH;
    Case.Ops.back().NumInputs = 1;
    Case.Ops.back().Inputs[0] =
        NdVar::scalar(Dispatch, I->Image.getPointerSize());
    Case.InstructionBoundaries.back().Control = LowInstructionControl::Branch;
    Case.InstructionBoundaries.back().ControlFlags =
        LowInstructionControlFlag::Branch;
    F.ModuleAnalysisRoots.insert(Case.StartAddr);
    F.OrdinaryModuleAnalysisRoots.insert(Case.StartAddr);
    Work = limits::kMaxJumpTableCandidateEvidenceWork;
    EXPECT_FALSE(CFGBuilder::proveInstructionLocalTableSource(
        I->Image, F, *source(F), Work));
    expectFirstProtected(*I);
  }
}

TEST_F(ModuleAdjacentTable, EncodedFieldMustParticipateInAddressOperation) {
  for (bool X64 : {false, true}) {
    auto I = fixture(X64);
    ASSERT_TRUE(I);
    auto &F = I->Functions[1];
    ASSERT_TRUE(source(F));
    const auto Original = *source(F);
    bool Changed = false;
    for (auto &B : F.Blocks)
      for (auto &Op : B.Ops)
        if (Op.Addr == Original.InstructionAddr && Op.Seq == Original.OpSeq) {
          ASSERT_EQ(Op.Opcode, NdOp::INT_ADD);
          ASSERT_EQ(Op.NumInputs, 2u);
          const auto Index = unsigned(Original.InputIndex);
          ASSERT_LT(Index, Op.NumInputs);
          Op.Inputs[2] = Op.Inputs[Index];
          Op.Inputs[Index].Provenance = ConstantAddressProvenance::Scalar;
          Op.Inputs[Index].AddressOwnerVA = InvalidVA;
          Op.NumInputs = 3;
          Changed = true;
        }
    ASSERT_TRUE(Changed);
    for (auto &S : F.RelocatedInstructionAddressOccurrences)
      if (S.InstructionAddr == Original.InstructionAddr &&
          S.OpSeq == Original.OpSeq)
        S.InputIndex = 2;
    size_t Work = limits::kMaxJumpTableCandidateEvidenceWork;
    EXPECT_FALSE(CFGBuilder::proveInstructionLocalTableSource(
        I->Image, F, *source(F), Work));
    expectFirstProtected(*I);
  }
}

TEST_F(ModuleAdjacentTable, IndependentDispatchRootCannotBypassIndexProof) {
  for (bool X64 : {false, true}) {
    auto I = fixture(X64, 1);
    ASSERT_TRUE(I);
    ASSERT_TRUE(splitDispatch(I->Functions[1], true));
    ASSERT_TRUE(source(I->Functions[1]));
    size_t Work = limits::kMaxJumpTableCandidateEvidenceWork;
    EXPECT_FALSE(CFGBuilder::proveInstructionLocalTableSource(
        I->Image, I->Functions[1], *source(I->Functions[1]), Work));
    expectFirstProtected(*I);
  }
}

TEST_F(ModuleAdjacentTable, PhysicalGapCannotEraseAFinalPredecessor) {
  for (bool X64 : {false, true}) {
    auto I = fixture(X64);
    ASSERT_TRUE(I);
    auto &F = I->Functions[1];
    ASSERT_TRUE(splitDispatch(F, false));
    size_t Work = limits::kMaxJumpTableCandidateEvidenceWork;
    ASSERT_TRUE(CFGBuilder::proveInstructionLocalTableSource(I->Image, F,
                                                             *source(F), Work));
    auto &Head = F.Blocks.front();
    ASSERT_FALSE(Head.InstructionBoundaries.empty());
    ASSERT_EQ(Head.InstructionBoundaries.back().OpCount, 1u);
    ASSERT_EQ(Head.Ops.back().Opcode, NdOp::NOP);
    Head.EndAddr = Head.InstructionBoundaries.back().Address;
    Head.Ops.pop_back();
    Head.InstructionBoundaries.pop_back();
    Work = limits::kMaxJumpTableCandidateEvidenceWork;
    EXPECT_FALSE(CFGBuilder::proveInstructionLocalTableSource(
        I->Image, F, *source(F), Work));
    expectFirstProtected(*I);
  }
}

TEST_F(ModuleAdjacentTable, FloatingTopClonesRemainDistinctContexts) {
  for (bool X64 : {false, true}) {
    auto I = fixture(X64, 2);
    ASSERT_TRUE(I);
    const auto &F = I->Functions[1];
    ASSERT_TRUE(source(F));
    std::set<va_t> Seen;
    bool Duplicated = false;
    for (const auto &B : F.Blocks)
      for (const auto &Boundary : B.InstructionBoundaries)
        Duplicated |= !Seen.insert(Boundary.Address).second;
    ASSERT_TRUE(Duplicated);
    size_t Work = limits::kMaxJumpTableCandidateEvidenceWork;
    EXPECT_FALSE(CFGBuilder::proveInstructionLocalTableSource(
        I->Image, F, *source(F), Work));
    expectFirstProtected(*I);
  }
}

TEST_F(ModuleAdjacentTable, IndependentSourceWithoutALoadRetainsPriorObject) {
  for (bool X64 : {false, true}) {
    auto I = fixture(X64);
    ASSERT_TRUE(I);
    auto Extra = sourceOnly(I->Functions[1]);
    ASSERT_FALSE(Extra.RelocatedInstructionAddressOccurrences.empty());
    ASSERT_TRUE(Extra.JumpTables.empty());
    for (const auto &B : Extra.Blocks)
      for (const auto &Op : B.Ops)
        ASSERT_NE(Op.Opcode, NdOp::LOAD);
    I->Functions.push_back(std::move(Extra));
    const auto Sources =
        I->Functions.back().RelocatedInstructionAddressOccurrences;
    I->Functions.back().RelocatedInstructionAddressOccurrences.clear();
    expectFirstUnprotected(*I);
    I->Functions.back().RelocatedInstructionAddressOccurrences = Sources;
    expectFirstProtected(*I);
    std::reverse(I->Functions.begin(), I->Functions.end());
    expectFirstProtected(*I);
  }
}

TEST_F(ModuleAdjacentTable, BothCSourceRoutesExecuteEveryMaskedIndex) {
  auto I = fixture(true);
  ASSERT_TRUE(I);
  for (unsigned Route : {0u, 1u, 2u}) {
    SCOPED_TRACE(Route);
    const auto C = tmpFile("adjacent.c");
    std::vector<std::string> Args{"decompile", I->Object.string(), "-o",
                                  C.string()};
    if (Route)
      Args.push_back("--llvm");
    if (Route == 2)
      Args.push_back("--no-opt");
    const auto Emitted = exec(ndBin(), Args);
    ASSERT_TRUE(Emitted.ok()) << Emitted.err;
    {
      std::ofstream Out(C, std::ios::app);
      Out << R"C(
int main(void) {
  const uint32_t inputs[] = {0, 1, 2, 3, UINT32_MAX, UINT32_C(0x80000000)};
  for (unsigned i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
    const uint32_t x = inputs[i];
    if (adjacent_first(x) != ((x & 1) ? 12 : 11) ||
        adjacent_second(x) != ((x & 1) ? 22 : 21))
      return 1;
  }
  return 0;
}
)C";
    }
    for (const char *Optimization : {"-O0", "-O2"}) {
      SCOPED_TRACE(Optimization);
      const auto Binary = tmpFile("adjacent-host");
      const auto Built =
          exec(NEVERD_TEST_CLANG,
               {Optimization, "-std=c11", C.string(), "-o", Binary.string()});
      ASSERT_TRUE(Built.ok()) << Built.err;
      const auto Observed = exec(Binary.string(), {});
      ASSERT_TRUE(Observed.ok()) << Observed.err;
    }
  }
}

} // namespace
