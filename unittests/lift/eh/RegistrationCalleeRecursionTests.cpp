//===- RegistrationCalleeRecursionTests.cpp - Mixed proof cycles ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep registration and x87 call proofs in one bounded invocation closure.

#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/support/BinaryEncoding.h"

#include <algorithm>
#include <optional>
#include <thread>
#include <vector>

using namespace neverd;

namespace {
BinaryImage makeRecursiveSEHImage(bool Cycle) {
  constexpr va_t Entry = 0x401000, Table = 0x403000;
  BinaryImage Image;
  Image.Arch = Arch::X86;
  Image.Bits = Bitness::Bits32;
  Image.Format = BinaryFormat::COFF;
  Image.Base = 0x400000;
  Image.Entry = Entry;
  Segment Text;
  Text.Name = ".text";
  Text.VA = Entry;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(0x110, 0xcc);
  const uint8_t Parent[] = {
      0x55, 0x8b, 0xec, 0x6a, 0xff, 0x68, 0,    0x30, 0x40, 0,
      0x68, 0x70, 0x10, 0x40, 0,    0x64, 0xa1, 0,    0,    0,
      0,    0x50, 0x64, 0x89, 0x25, 0,    0,    0,    0,    0xc7,
      0x45, 0xfc, 0,    0,    0,    0, // Activate the finally scope.
      0xe8, 0xd7, 0,    0,    0,       // Call the ordinary wrapper.
      0xc7, 0x45, 0xfc, 0xff, 0xff, 0xff, 0xff, 0xe8, 0x2b, 0,
      0,    0, // Call the finally on ordinary exit.
      0x8b, 0x4d, 0xf0, 0x64, 0x89, 0x0d, 0,    0,    0,    0,
      0x8b, 0xe5, 0x5d, 0xc3};
  std::copy(std::begin(Parent), std::end(Parent), Text.Data.begin());
  Text.Data[0x60] = 0xc3; // Separate finally invocation.
  Text.Data[0x70] = 0xc3; // Personality reference, not callable authority.
  if (Cycle) {
    // This wrapper cannot borrow its own pending ESP answer: the argument
    // remains below entry ESP when the parent returns with zero cleanup.
    const uint8_t Wrapper[] = {0x6a, 0, 0xe8, 0xf9, 0xfe, 0xff, 0xff, 0xc3};
    std::copy(std::begin(Wrapper), std::end(Wrapper),
              Text.Data.begin() + 0x100);
  } else
    Text.Data[0x100] = 0xc3;
  Text.Size = Text.FileSz = Text.Data.size();
  Section Code;
  Code.Name = Text.Name;
  Code.VA = Entry;
  Code.Size = Code.FileSz = Text.Size;
  Code.Flags = Text.Flags;
  Image.Sections.push_back(Code);
  Image.Segments.push_back(std::move(Text));
  Segment ScopeTable;
  ScopeTable.Name = ".rdata";
  ScopeTable.VA = Table;
  ScopeTable.Flags = SegmentFlags::Readable;
  ScopeTable.Data.resize(12);
  writeLE<uint32_t>(ScopeTable.Data.data(), UINT32_MAX);
  writeLE<uint32_t>(ScopeTable.Data.data() + 8, Entry + 0x60);
  ScopeTable.Size = ScopeTable.FileSz = ScopeTable.Data.size();
  Image.Segments.push_back(std::move(ScopeTable));
  Image.Symbols.push_back(Symbol::makeFunc(Entry, 0x80));
  Image.Symbols.push_back(Symbol::makeFunc(Entry + 0x100, 0x10));
  ExceptionFunction EH;
  EH.CodeRange = {Entry, Entry + 0x80};
  EH.Personality = ExceptionPersonality::ExceptHandler3;
  EH.Encoding = ExceptionEncoding::X86ScopeTableEH3;
  auto &Chain = EH.Registration.emplace();
  Chain.HandlerVA = Entry + 0x70;
  Chain.ScopeTableVA = Table;
  Chain.SeededTryLevel = -1;
  Chain.TryLevelOffset = -4;
  Chain.RegistrationOffset = -16;
  Chain.ChainInstallVA = Entry + 0x16;
  Chain.ChainRemoveVA = Entry + 0x38;
  Chain.TryLevelStores = {{Entry + 0x1d, Entry + 0x24, 0},
                          {Entry + 0x29, Entry + 0x30, -1}};
  Chain.Scopes = {{-1, 0, Entry + 0x60, true}};
  Image.ExceptionMetadata.Functions.push_back(EH);
  Image.ExceptionMetadata.rebuildIndex();
  return Image;
}
} // namespace

TEST(RegistrationCallABI, FloatingAndRegistrationProofsSharePendingCallees) {
  for (bool Cycle : {false, true}) {
    SCOPED_TRACE(Cycle);
    auto Image = makeRecursiveSEHImage(Cycle);
    size_t Work = 0;
    // The parent's explicit EBP restoration proves only its own RET. The
    // recursive wrapper still has no balanced returning-stack contract.
    EXPECT_EQ(getCheckedX86CalleeStackPop(Image, Image.Entry, &Work), 0u);
    EXPECT_LE(Work, limits::kMaxRegistrationEHStateWork);
    Work = 0;
    EXPECT_EQ(getCheckedX86CalleeStackPop(Image, Image.Entry + 0x100, &Work),
              Cycle ? std::nullopt : std::optional<uint32_t>{0});
    EXPECT_LE(Work, limits::kMaxRegistrationEHStateWork);
  }
}

namespace {
BinaryImage makeSeparateStackEntryImage(bool Reached, bool Cached,
                                        const char *ImportName = "Sleep") {
  constexpr va_t TextVA = 0x401000, DataVA = 0x404000, IAT = DataVA + 16;
  BinaryImage Image;
  Image.Arch = Arch::X86;
  Image.Bits = Bitness::Bits32;
  Image.Format = BinaryFormat::COFF;
  Image.Base = 0x400000;
  Image.Entry = TextVA;
  Segment Text;
  Text.Name = ".text";
  Text.VA = TextVA;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(0x80, 0xcc);
  const uint8_t Caller[] = {0xc6, 0x05, 0, 0, 0, 0, 0,   0x6a,
                            0,    0xe8, 0, 0, 0, 0, 0xc3};
  std::copy(std::begin(Caller), std::end(Caller), Text.Data.begin());
  writeLE<uint32_t>(Text.Data.data() + 2, IAT);
  writeLE<uint32_t>(Text.Data.data() + 10, 0x40 - 14);
  const uint8_t Child[] = {0x85, 0xc0, 0x75, 0x1c, 0xc2, 4, 0};
  std::copy(std::begin(Child), std::end(Child), Text.Data.begin() + 0x40);
  if (!Reached) {
    Text.Data[0x40] = 0xc2;
    Text.Data[0x41] = 4;
    Text.Data[0x42] = 0;
  }
  std::vector<uint8_t> Other;
  if (Cached) {
    Other = {0x8b, 0x15, 0, 0, 0, 0, 0x6a, 0, 0xff, 0xd2, 0xc2, 4, 0};
    writeLE<uint32_t>(Other.data() + 2, IAT);
  } else {
    Other = {0x6a, 0, 0xff, 0x15, 0, 0, 0, 0, 0xc2, 4, 0};
    writeLE<uint32_t>(Other.data() + 4, IAT);
  }
  std::copy(Other.begin(), Other.end(), Text.Data.begin() + 0x60);
  Text.Size = Text.FileSz = Text.Data.size();
  Image.Segments.push_back(std::move(Text));
  Segment Pointers;
  Pointers.Name = ".rdata";
  Pointers.VA = 0x403000;
  Pointers.Flags = SegmentFlags::Readable;
  Pointers.Data.resize(4);
  writeLE<uint32_t>(Pointers.Data.data(), TextVA + 0x60);
  Pointers.Size = Pointers.FileSz = Pointers.Data.size();
  Image.CodePtrRelocSlots.insert(Pointers.VA);
  Image.Segments.push_back(std::move(Pointers));
  Segment Data;
  Data.Name = ".data";
  Data.VA = DataVA;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.resize(32);
  Data.Size = Data.FileSz = Data.Data.size();
  Image.Segments.push_back(std::move(Data));
  for (const auto &Segment : Image.Segments) {
    Section Code;
    Code.Name = Segment.Name;
    Code.VA = Segment.VA;
    Code.Size = Code.FileSz = Segment.Size;
    Code.Flags = Segment.Flags;
    Image.Sections.push_back(Code);
  }
  Image.Symbols.push_back(Symbol::makeFunc(TextVA, sizeof(Caller)));
  Image.Symbols.push_back(Symbol::makeFunc(TextVA + 0x40, 0x30));
  Image.Imports.push_back({"KERNEL32.dll", ImportName, 0, IAT});
  EXPECT_TRUE(Image.recordImportStorageSlot(
      IAT, ImportName, 0, ImportStorageEvidence::ImportDirectory));
  return Image;
}
LowFunc stackEntryQuery(va_t Target) {
  LowFunc Query;
  Query.Entry = 0x401000;
  Query.Blocks.resize(1);
  LowOp Call;
  Call.Opcode = NdOp::CALL;
  Call.addInput(NdVar::cst(Target, 4));
  Query.Blocks.front().Ops.push_back(Call);
  return Query;
}

BinaryImage makeRepeatedLocalStackImage(unsigned Levels, bool Cycle = false) {
  auto Image = makeSeparateStackEntryImage(false, false, "EncodePointer");
  auto &Text = Image.Segments.front();
  Text.Data.assign(0x40 + 16 * Levels, 0xcc);
  auto &Pointers = Image.Segments[1];
  Pointers.Data.resize(4 * Levels);
  Image.CodePtrRelocSlots.clear();
  for (unsigned I = 0; I != Levels; ++I) {
    const size_t Offset = 0x40 + 16 * I;
    writeLE<uint32_t>(Pointers.Data.data() + 4 * I, Text.VA + Offset);
    Image.CodePtrRelocSlots.insert(Pointers.VA + 4 * I);
    if (I + 1 == Levels && !Cycle) {
      Text.Data[Offset] = 0xc3;
      continue;
    }
    const va_t Target = Text.VA + (I + 1 == Levels ? 0x40 : Offset + 16);
    for (unsigned Call : {0u, 5u}) {
      Text.Data[Offset + Call] = 0xe8;
      writeLE<uint32_t>(Text.Data.data() + Offset + Call + 1,
                        uint32_t(Target - (Text.VA + Offset + Call + 5)));
    }
    Text.Data[Offset + 10] = 0xc3;
  }
  for (size_t I = 0; I != Image.Segments.size(); ++I) {
    auto &Segment = Image.Segments[I];
    Segment.Size = Segment.FileSz = Segment.Data.size();
    Image.Sections[I].Size = Image.Sections[I].FileSz = Segment.Size;
  }
  Image.Entry = Text.VA + 0x40;
  Image.Symbols = {Symbol::makeFunc(Image.Entry, 16 * Levels)};
  return Image;
}
} // namespace

TEST(RegistrationCalleeRecursion,
     RepeatedLocalStackWitnessesStayInTheirParent) {
  for (bool ImportedLeaf : {false, true}) {
    SCOPED_TRACE(ImportedLeaf);
    auto Image = makeRepeatedLocalStackImage(10);
    if (ImportedLeaf) {
      // Every completed local proof must keep this dependency when reused.
      const uint8_t Leaf[] = {0x6a, 0, 0xff, 0x15, 0x10, 0x40, 0x40, 0, 0xc3};
      auto &Bytes = Image.Segments.front().Data;
      std::copy(std::begin(Leaf), std::end(Leaf), Bytes.end() - 16);
    }
    Decoder Decode;
    ASSERT_TRUE(Decode.init(Image));
    auto Low = CFGBuilder().buildIntegerFrameProof(Image, Decode, Image.Entry,
                                                   "local-stack-diamond");
    ASSERT_TRUE(Low.hasCompleteLiftCoverage());
    for (bool Reversed : {false, true}) {
      SCOPED_TRACE(Reversed);
      if (Reversed)
        std::reverse(Low.Blocks.begin(), Low.Blocks.end());
      size_t Work = 0;
      RegistrationCallCalleeIndex Index(Image, &Work);
      const auto Contracts = Index.stackContracts(Low);
      ASSERT_TRUE(Contracts);
      ASSERT_EQ(Contracts->size(), ImportedLeaf ? 10u : 9u);
      for (const auto &Contract : *Contracts) {
        EXPECT_EQ(Contract.StackPopBytes, Contract.Indirect ? 4u : 0u);
        if (!Contract.Indirect)
          EXPECT_EQ(Contract.RequiredImports, ImportedLeaf
                                                  ? std::vector<va_t>{0x404010}
                                                  : std::vector<va_t>{});
        EXPECT_TRUE(Contract.PreservesImportBindings);
      }
      EXPECT_LT(Work, limits::kMaxRegistrationEHStateWork);
    }
  }
}

TEST(RegistrationCalleeRecursion, LocalStackWitnessesDoNotCompleteCycles) {
  for (bool Cycle : {false, true}) {
    SCOPED_TRACE(Cycle);
    auto Image = makeRepeatedLocalStackImage(3, Cycle);
    if (!Cycle) {
      auto &Bytes = Image.Segments.front().Data;
      Bytes[Bytes.size() - 16] = 0xc2;
      Bytes[Bytes.size() - 15] = 4;
      Bytes[Bytes.size() - 14] = 0;
    }
    Decoder Decode;
    ASSERT_TRUE(Decode.init(Image));
    const auto Low = CFGBuilder().buildIntegerFrameProof(
        Image, Decode, Image.Entry, "invalid-local-stack");
    ASSERT_TRUE(Low.hasCompleteLiftCoverage());
    RegistrationCallCalleeIndex Index(Image);
    const auto Contracts = Index.stackContracts(Low);
    ASSERT_TRUE(Contracts);
    if (Cycle) {
      EXPECT_TRUE(Contracts->empty());
    } else {
      ASSERT_EQ(Contracts->size(), 1u);
      EXPECT_EQ(Contracts->front().Target, Image.Entry + 32);
      EXPECT_EQ(Contracts->front().StackPopBytes, 4u);
    }
  }
}

TEST(RegistrationCalleeRecursion, PendingLocalDependencyCanBeRetried) {
  auto Image = makeRepeatedLocalStackImage(3);
  auto &Bytes = Image.Segments.front().Data;
  // A restores its own ESP even while B has no returning contract. B's first
  // query therefore fails under the pending A, but must succeed after A has
  // finished. Memoizing that incomplete refusal would lose B's valid result.
  const uint8_t A[] = {0x55, 0x89, 0xe5, 0xe8, 8,    0,
                       0,    0,    0x89, 0xec, 0x5d, 0xc3};
  const uint8_t B[] = {0xe8, 0xeb, 0xff, 0xff, 0xff, 0xc3};
  std::copy(std::begin(A), std::end(A), Bytes.begin() + 0x50);
  std::copy(std::begin(B), std::end(B), Bytes.begin() + 0x60);
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Image));
  const auto Low = CFGBuilder().buildIntegerFrameProof(
      Image, Decode, Image.Entry, "pending-local-stack");
  ASSERT_TRUE(Low.hasCompleteLiftCoverage());
  RegistrationCallCalleeIndex Index(Image);
  const auto Contracts = Index.stackContracts(Low);
  ASSERT_TRUE(Contracts);
  ASSERT_EQ(Contracts->size(), 2u);
  for (const auto &Contract : *Contracts) {
    EXPECT_EQ(Contract.StackPopBytes, 0u);
    EXPECT_TRUE(Contract.RequiredImports.empty());
  }
}

TEST(RegistrationCalleeRecursion, LocalStackCoordinatesNeedAProvenRestore) {
  for (unsigned Variant : {0u, 1u, 2u}) {
    SCOPED_TRACE(Variant);
    auto Image = makeRepeatedLocalStackImage(3);
    auto &Bytes = Image.Segments.front().Data;
    std::vector<uint8_t> Body;
    if (Variant) {
      if (Variant == 2)
        Body.push_back(0x66); // A narrowed save cannot restore entry ESP.
      Body.insert(Body.end(), {0x89, 0xe7}); // mov (e)di, (e)sp
    }
    Body.insert(Body.end(), {0x8b, 0x20}); // mov esp, [eax]
    if (Variant)
      Body.insert(Body.end(), {0x89, 0xfc}); // mov esp, edi
    Body.push_back(0xe8);
    const size_t Displacement = Body.size();
    Body.resize(Body.size() + 4);
    writeLE<uint32_t>(Body.data() + Displacement, 16 - Body.size());
    Body.push_back(0xc3);
    std::copy(Body.begin(), Body.end(), Bytes.begin() + 0x50);
    Decoder Decode;
    ASSERT_TRUE(Decode.init(Image));
    const auto Low = CFGBuilder().buildIntegerFrameProof(
        Image, Decode, Image.Entry, "local-stack-restore");
    ASSERT_TRUE(Low.hasCompleteLiftCoverage());
    RegistrationCallCalleeIndex Index(Image);
    const auto Contracts = Index.stackContracts(Low);
    ASSERT_TRUE(Contracts);
    ASSERT_EQ(Contracts->size(), Variant == 1 ? 2u : 1u);
    for (const auto &Contract : *Contracts)
      EXPECT_EQ(Contract.StackPopBytes, 0u);
  }
}

TEST(RegistrationCalleeRecursion, StackImportsBelongToTheQueriedEntry) {
  for (bool Reached : {false, true})
    for (bool Cached : {false, true}) {
      SCOPED_TRACE(::testing::Message() << Reached << "/" << Cached);
      auto Image = makeSeparateStackEntryImage(Reached, Cached);
      Decoder Decode;
      ASSERT_TRUE(Decode.init(Image));
      const auto Low =
          CFGBuilder().buildIntegerFrameProof(Image, Decode, 0x401040, "child");
      ASSERT_TRUE(Low.hasCompleteLiftCoverage());
      ASSERT_TRUE(
          std::any_of(Low.Blocks.begin(), Low.Blocks.end(),
                      [](const auto &B) { return B.StartAddr == 0x401060; }));
      ASSERT_TRUE(Low.ModuleAnalysisRoots.count(0x401060));
      size_t Work = 0;
      RegistrationCallCalleeIndex Index(Image, &Work);
      for (va_t Target : {va_t{0x401040}, va_t{0x401060}, va_t{0x401040}}) {
        SCOPED_TRACE(Target);
        const auto Contracts = Index.stackContracts(stackEntryQuery(Target));
        ASSERT_TRUE(Contracts);
        ASSERT_EQ(Contracts->size(), 1u);
        EXPECT_EQ(Contracts->front().StackPopBytes, 4u);
        EXPECT_EQ(Contracts->front().RequiredImports,
                  Reached || Target == 0x401060 ? std::vector<va_t>{0x404010}
                                                : std::vector<va_t>{});
      }
      EXPECT_LT(Work, limits::kMaxRegistrationEHStateWork);
      size_t Used = 0;
      EXPECT_EQ(getCheckedX86CalleeStackPop(Image, 0x401040, &Used), 4u);
      ASSERT_GT(Used, 0u);
      ASSERT_LT(Used, limits::kMaxRegistrationEHStateWork);
      Work = limits::kMaxRegistrationEHStateWork - Used;
      EXPECT_EQ(getCheckedX86CalleeStackPop(Image, 0x401040, &Work), 4u);
      EXPECT_EQ(Work, limits::kMaxRegistrationEHStateWork);
      Work = limits::kMaxRegistrationEHStateWork - Used + 1;
      EXPECT_FALSE(getCheckedX86CalleeStackPop(Image, 0x401040, &Work));
      EXPECT_EQ(Work, limits::kMaxRegistrationEHStateWork);
      EXPECT_EQ(getCheckedX86CalleeStackPop(Image, Image.Entry),
                Reached ? std::nullopt : std::optional<uint32_t>{0});
    }
}

TEST(RegistrationCalleeRecursion, IndependentStackMemoKeepsImportsAndBudgets) {
  for (bool Reached : {false, true})
    for (bool Cached : {false, true}) {
      SCOPED_TRACE(::testing::Message() << Reached << "/" << Cached);
      const auto Image = makeSeparateStackEntryImage(Reached, Cached);
      constexpr va_t Target = 0x401040;
      size_t FreshWork = 0;
      ASSERT_EQ(getCheckedX86CalleeStackPop(Image, Target, &FreshWork), 4u);
      ASSERT_GT(FreshWork, 1u);
      ASSERT_LT(FreshWork, limits::kMaxRegistrationEHStateWork);
      const auto Independent =
          Reached ? std::nullopt : std::optional<uint32_t>{4};
      size_t IndependentWork = 0;
      ASSERT_EQ(getCheckedX86IndependentCalleeStackPop(
                    Image, Target, IndependentWork,
                    limits::kMaxRegistrationEHStateWork),
                Independent);
      ASSERT_GE(IndependentWork, FreshWork);
      for (bool IndependentFirst : {false, true}) {
        SCOPED_TRACE(IndependentFirst);
        RegistrationStackPopMemo Memo(Image);
        RegistrationStackPopMemoScope Scope(Memo);
        size_t Work = 0;
        if (IndependentFirst)
          EXPECT_EQ(
              getCheckedX86IndependentCalleeStackPop(
                  Image, Target, Work, limits::kMaxRegistrationEHStateWork),
              Independent);
        else
          EXPECT_EQ(getCheckedX86CalleeStackPop(Image, Target, &Work), 4u);
        EXPECT_EQ(Work, IndependentFirst ? IndependentWork : FreshWork);
        const auto Known = Memo.find(Image, Target, 0);
        ASSERT_TRUE(Known);
        EXPECT_EQ(Known->Pop, 4u);
        EXPECT_EQ(Known->Work, FreshWork);

        // Import-dependent returning ESP remains useful to the ordinary
        // caller, but cannot become an independent jump-table fact. The
        // optional opaque-call projection charges its own bounded work;
        // neither entry point changes the ordinary memo's cost or premises.
        Work = 0;
        EXPECT_EQ(getCheckedX86CalleeStackPop(Image, Target, &Work), 4u);
        EXPECT_EQ(Work, FreshWork);
        for (size_t Before : {size_t{0}, size_t{17}})
          for (size_t Remaining : {size_t{0}, size_t{1}, IndependentWork - 1,
                                   IndependentWork, IndependentWork + 1}) {
            Work = Before;
            const auto Actual = getCheckedX86IndependentCalleeStackPop(
                Image, Target, Work, Before + Remaining);
            EXPECT_EQ(Actual, Remaining >= IndependentWork ? Independent
                                                           : std::nullopt);
            EXPECT_EQ(Work, Before + std::min(Remaining, IndependentWork));
          }
      }
      // A short attempt must not poison a later, adequately funded query.
      RegistrationStackPopMemo Memo(Image);
      RegistrationStackPopMemoScope Scope(Memo);
      size_t Work = 0;
      EXPECT_FALSE(
          getCheckedX86IndependentCalleeStackPop(Image, Target, Work, 1));
      EXPECT_EQ(Work, 1u);
      EXPECT_FALSE(Memo.find(Image, Target, 0));
      Work = 0;
      EXPECT_EQ(getCheckedX86IndependentCalleeStackPop(Image, Target, Work,
                                                       IndependentWork + 1),
                Independent);
      EXPECT_EQ(Work, IndependentWork);
      EXPECT_TRUE(Memo.find(Image, Target, 0));
    }
}

namespace {
BinaryImage makeScalarStackBudgetImage(size_t Instructions) {
  BinaryImage Image;
  Image.Arch = Arch::X86;
  Image.Bits = Bitness::Bits32;
  Image.Format = BinaryFormat::COFF;
  Image.Base = 0x400000;
  Image.Entry = 0x401000;
  Segment Text;
  Text.Name = ".text";
  Text.VA = Image.Entry;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  // Repeated ADDs preserve ESP, but their complete integer-frame proof must
  // still charge every operation. No missing bytes or unsupported opcodes
  // supply the refusal at the full work ceiling.
  for (size_t I = 0; I < Instructions; ++I)
    Text.Data.insert(Text.Data.end(), {0x01, 0xd8}); // add eax, ebx
  Text.Data.insert(Text.Data.end(), {0xc2, 4, 0});   // ret 4
  Text.Size = Text.FileSz = Text.Data.size();
  Section Code;
  Code.Name = Text.Name;
  Code.VA = Text.VA;
  Code.Size = Code.FileSz = Text.Size;
  Code.Flags = Text.Flags;
  Image.Symbols.push_back(Symbol::makeFunc(Image.Entry, Text.Size));
  Image.Sections.push_back(Code);
  Image.Segments.push_back(std::move(Text));
  return Image;
}
} // namespace

TEST(RegistrationCalleeRecursion, FullBudgetRefusalKeepsTheExactAllowance) {
  const auto Image = makeScalarStackBudgetImage(32768);
  const va_t Target = Image.Entry;
  constexpr size_t Limit = limits::kMaxRegistrationEHStateWork;
  size_t FreshWork = 0;
  ASSERT_FALSE(getCheckedX86CalleeStackPop(Image, Target, &FreshWork));
  ASSERT_EQ(FreshWork, Limit);

  RegistrationStackPopMemo Memo(Image);
  {
    RegistrationStackPopMemoScope Scope(Memo);
    size_t Work = 0;
    EXPECT_FALSE(getCheckedX86CalleeStackPop(Image, Target, &Work));
    EXPECT_EQ(Work, FreshWork);
    const auto Known = Memo.find(Image, Target, 0);
    ASSERT_TRUE(Known);
    EXPECT_FALSE(Known->Pop);
    EXPECT_EQ(Known->Work, Limit);
    EXPECT_FALSE(Known->HasImportDependencies);

    Work = 0;
    EXPECT_FALSE(getCheckedX86CalleeStackPop(Image, Target, &Work));
    EXPECT_EQ(Work, FreshWork);
    // An independent caller embeds its remaining allowance in the callee's
    // account. Its own prior work must be retained when the refusal replays.
    Work = 19;
    EXPECT_FALSE(getCheckedX86IndependentCalleeStackPop(Image, Target, Work,
                                                        Limit + 19));
    EXPECT_EQ(Work, Limit + 19);

    // Identical addresses in another image still get that image's answer.
    const auto Small = makeScalarStackBudgetImage(1);
    Work = 0;
    EXPECT_EQ(getCheckedX86CalleeStackPop(Small, Target, &Work), 4u);
    EXPECT_LT(Work, Limit);
    EXPECT_FALSE(Memo.find(Small, Target, 0));
  }

  // A refusal at the full allowance cannot replace a query that began with
  // less. Compare both the result and the exact charged work with fresh runs.
  for (size_t Before : {size_t{1}, Limit - 1}) {
    SCOPED_TRACE(Before);
    FreshWork = Before;
    const auto Fresh = getCheckedX86CalleeStackPop(Image, Target, &FreshWork);
    RegistrationStackPopMemoScope Scope(Memo);
    size_t Work = Before;
    EXPECT_EQ(getCheckedX86CalleeStackPop(Image, Target, &Work), Fresh);
    EXPECT_EQ(Work, FreshWork);
  }
}

namespace {
void expectSameStackContract(
    const std::optional<RegistrationCalleeStackContract> &Actual,
    const std::optional<RegistrationCalleeStackContract> &Expected) {
  ASSERT_EQ(bool(Actual), bool(Expected));
  if (!Expected)
    return;
  EXPECT_EQ(Actual->Target, Expected->Target);
  EXPECT_EQ(Actual->StackPopBytes, Expected->StackPopBytes);
  EXPECT_EQ(Actual->Indirect, Expected->Indirect);
  EXPECT_EQ(Actual->Loads, Expected->Loads);
  EXPECT_EQ(Actual->RequiredImports, Expected->RequiredImports);
  EXPECT_EQ(Actual->ImportVeneer, Expected->ImportVeneer);
  EXPECT_EQ(Actual->PreservesImportBindings, Expected->PreservesImportBindings);
}
} // namespace

TEST(RegistrationCalleeRecursion, RawGraphsKeepInvocationImportsAndWork) {
  for (bool Reached : {false, true})
    for (bool Cached : {false, true}) {
      SCOPED_TRACE(::testing::Message() << Reached << "/" << Cached);
      const auto Image = makeSeparateStackEntryImage(Reached, Cached);
      constexpr va_t Target = 0x401040;
      size_t FreshWork = 0;
      const auto Fresh =
          RegistrationCallCalleeIndex(Image, &FreshWork).stackContract(Target);
      ASSERT_TRUE(Fresh);
      EXPECT_EQ(Fresh->RequiredImports,
                Reached ? std::vector<va_t>{0x404010} : std::vector<va_t>{});
      ASSERT_GT(FreshWork, 1u);
      ASSERT_LT(FreshWork, limits::kMaxRegistrationEHStateWork);
      RegistrationStackPopMemo Memo(Image);
      for (unsigned Run = 0; Run < 3; ++Run) {
        SCOPED_TRACE(Run);
        RegistrationStackPopMemoScope Scope(Memo);
        size_t Work = 0;
        expectSameStackContract(
            RegistrationCallCalleeIndex(Image, &Work).stackContract(Target),
            Fresh);
        EXPECT_EQ(Work, FreshWork);
        ASSERT_TRUE(Memo.findGraph(Image, Target));
        // A new invocation owner reads the raw graph but cannot borrow a
        // completed proof from the standalone-answer memo.
        EXPECT_FALSE(Memo.find(Image, Target, 0));
      }
      const auto Graph = Memo.findGraph(Image, Target);
      for (size_t Before : {limits::kMaxRegistrationEHStateWork - FreshWork,
                            limits::kMaxRegistrationEHStateWork - FreshWork + 1,
                            limits::kMaxRegistrationEHStateWork - 1}) {
        size_t ExpectedWork = Before;
        const auto Expected = RegistrationCallCalleeIndex(Image, &ExpectedWork)
                                  .stackContract(Target);
        RegistrationStackPopMemoScope Scope(Memo);
        size_t Work = Before;
        expectSameStackContract(
            RegistrationCallCalleeIndex(Image, &Work).stackContract(Target),
            Expected);
        EXPECT_EQ(Work, ExpectedWork);
        EXPECT_EQ(Memo.findGraph(Image, Target), Graph);
      }
    }
}

TEST(RegistrationCalleeRecursion, RawGraphsCannotCertifyPendingCycles) {
  for (bool Cycle : {false, true}) {
    const auto Image = makeRecursiveSEHImage(Cycle);
    RegistrationStackPopMemo Memo(Image);
    for (va_t Target : {Image.Entry, Image.Entry + 0x100}) {
      SCOPED_TRACE(::testing::Message() << Cycle << "/" << Target);
      size_t FreshWork = 0;
      const auto Fresh =
          RegistrationCallCalleeIndex(Image, &FreshWork).stackContract(Target);
      if (Target == Image.Entry || !Cycle)
        ASSERT_TRUE(Fresh);
      else
        ASSERT_FALSE(Fresh);
      for (unsigned Run = 0; Run < 2; ++Run) {
        RegistrationStackPopMemoScope Scope(Memo);
        size_t Work = 0;
        expectSameStackContract(
            RegistrationCallCalleeIndex(Image, &Work).stackContract(Target),
            Fresh);
        EXPECT_EQ(Work, FreshWork);
      }
    }
    EXPECT_FALSE(Memo.findGraph(Image, Image.Entry));
    EXPECT_TRUE(Memo.findGraph(Image, Image.Entry + 0x100));
  }
}

TEST(RegistrationCalleeRecursion, RawGraphsFollowImageAndScopeLifetime) {
  auto Image = makeScalarStackBudgetImage(1);
  auto Other = makeScalarStackBudgetImage(1);
  Other.Segments[0].Data[Other.Segments[0].Data.size() - 2] = 8;
  const va_t Target = Image.Entry;
  size_t FreshWork = 0;
  ASSERT_EQ(getCheckedX86CalleeStackPop(Image, Target, &FreshWork), 4u);
  // A disabled or insufficient storage allowance only changes retention.
  for (size_t Bytes : {size_t{0}, size_t{1}, sizeof(LowFunc)}) {
    RegistrationStackPopMemo Memo(Image, Bytes);
    RegistrationStackPopMemoScope Scope(Memo);
    for (unsigned Run = 0; Run < 2; ++Run) {
      size_t Work = 0;
      const auto Result =
          RegistrationCallCalleeIndex(Image, &Work).stackContract(Target);
      ASSERT_TRUE(Result);
      EXPECT_EQ(Result->StackPopBytes, 4u);
      EXPECT_EQ(Work, FreshWork);
      EXPECT_FALSE(Memo.findGraph(Image, Target));
    }
  }
  {
    RegistrationStackPopMemo Memo(Image), OtherMemo(Other);
    RegistrationStackPopMemoScope Scope(Memo);
    ASSERT_EQ(getCheckedX86CalleeStackPop(Image, Target), 4u);
    const auto Graph = Memo.findGraph(Image, Target);
    ASSERT_TRUE(Graph);
    EXPECT_EQ(getCheckedX86CalleeStackPop(Other, Target), 8u);
    EXPECT_FALSE(Memo.findGraph(Other, Target));
    {
      RegistrationStackPopMemoScope OtherScope(OtherMemo);
      EXPECT_EQ(getCheckedX86CalleeStackPop(Other, Target), 8u);
      EXPECT_TRUE(OtherMemo.findGraph(Other, Target));
      EXPECT_EQ(getCheckedX86CalleeStackPop(Image, Target), 4u);
      EXPECT_FALSE(OtherMemo.findGraph(Image, Target));
    }
    size_t Work = 0;
    ASSERT_TRUE(
        RegistrationCallCalleeIndex(Image, &Work).stackContract(Target));
    EXPECT_EQ(Work, FreshWork);
    EXPECT_EQ(Memo.findGraph(Image, Target), Graph);
  }
  // Publication or editing retires the whole memo before the same image
  // object can be reused. Address identity alone must not retain old bytes.
  Image.Segments[0].Data[Image.Segments[0].Data.size() - 2] = 8;
  RegistrationStackPopMemo Memo(Image);
  RegistrationStackPopMemoScope Scope(Memo);
  EXPECT_EQ(getCheckedX86CalleeStackPop(Image, Target), 8u);
  EXPECT_TRUE(Memo.findGraph(Image, Target));
}

TEST(RegistrationCalleeRecursion, RawGraphsExcludeExceptionProofState) {
  const auto Image = makeScalarStackBudgetImage(1);
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Image));
  const auto Plain =
      CFGBuilder().buildIntegerFrameProof(Image, Dec, Image.Entry, "abi-stack");
  ASSERT_FALSE(Plain.Blocks.empty());
  ASSERT_TRUE(Plain.hasCompleteLiftCoverage());
  for (unsigned Variant = 0; Variant < 6; ++Variant) {
    SCOPED_TRACE(Variant);
    auto Graph = Plain;
    if (Variant == 0)
      Graph.ExceptionMetadata.emplace();
    else if (Variant == 1)
      Graph.RegistrationStates.emplace();
    else if (Variant == 2)
      Graph.Blocks.front().ExceptionalSuccs.emplace_back();
    else if (Variant == 3)
      Graph.Blocks.front().ExceptionalPreds.emplace_back();
    else if (Variant == 4)
      ++Graph.Entry;
    else
      Graph.Blocks.clear();
    RegistrationStackPopMemo Memo(Image);
    Memo.recordGraph(Image, Image.Entry, Graph);
    EXPECT_FALSE(Memo.findGraph(Image, Image.Entry));
    Memo.recordGraph(Image, Image.Entry, Plain);
    EXPECT_TRUE(Memo.findGraph(Image, Image.Entry));
  }
}

TEST(RegistrationCalleeRecursion, SharedRawGraphsKeepWorkerBudgetsSeparate) {
  const auto Image = makeSeparateStackEntryImage(true, true);
  constexpr va_t Target = 0x401040;
  size_t FreshWork = 0;
  const auto Fresh =
      RegistrationCallCalleeIndex(Image, &FreshWork).stackContract(Target);
  ASSERT_TRUE(Fresh);
  ASSERT_GT(FreshWork, 1u);
  ASSERT_LT(FreshWork, limits::kMaxRegistrationEHStateWork);
  RegistrationStackPopMemo Memo(Image);
  const auto Query = [&](bool Short) {
    RegistrationStackPopMemoScope Scope(Memo);
    for (unsigned Run = 0; Run < 8; ++Run) {
      size_t Work =
          Short ? limits::kMaxRegistrationEHStateWork - FreshWork + 1 : 0;
      const auto Actual =
          RegistrationCallCalleeIndex(Image, &Work).stackContract(Target);
      expectSameStackContract(Actual, Short ? std::nullopt : Fresh);
      EXPECT_EQ(Work, Short ? limits::kMaxRegistrationEHStateWork : FreshWork);
    }
  };
  std::thread Full(Query, false), Short(Query, true);
  Full.join();
  Short.join();
  EXPECT_TRUE(Memo.findGraph(Image, Target));
}
