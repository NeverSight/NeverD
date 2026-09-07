//===- GoRuntimeMalformedTests.cpp - Go malformed pclntab record tests -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "GoRuntimeEHTestsDetail.h"
#include "gtest/gtest.h"

namespace {

using namespace neverd;
using namespace neverd::go_loader;
using namespace neverd::go_eh_test;

//===----------------------------------------------------------------------===//
// Malformed records
//===----------------------------------------------------------------------===//

// Relocate the single record so HighBytes end at the address limit, with the
// remaining bytes mapped at zero. Only the functab pointer changes; names,
// pc-value tables, and stack-map payloads keep their valid original addresses.
void installRecordAtAddressLimit(GoTestImage &T, BuiltPclnTab Tab,
                                 uint32_t Magic, size_t HighBytes) {
  const size_t RecordOffset = Tab.RecordOffsets[0];
  ASSERT_GT(HighBytes, 0u);
  ASSERT_LE(HighBytes, Tab.Bytes.size() - RecordOffset);
  const va_t RecordVA = InvalidVA - (HighBytes - 1);
  uint64_t FuncTabOffset = 16;
  if (Magic != kGo12Magic)
    std::memcpy(&FuncTabOffset, Tab.Bytes.data() + 56, sizeof(FuncTabOffset));
  const va_t RecordBase = kPclnVA + (Magic == kGo12Magic ? 0 : FuncTabOffset);
  Tab.put64(FuncTabOffset + 8, RecordVA - RecordBase);
  T.installPclnTab(Tab.Bytes);

  Segment High;
  High.VA = RecordVA;
  High.Size = HighBytes;
  High.Data.assign(Tab.Bytes.begin() + RecordOffset,
                   Tab.Bytes.begin() + RecordOffset + HighBytes);
  T.Img.Segments.push_back(std::move(High));
  if (RecordOffset + HighBytes < Tab.Bytes.size()) {
    Segment Low;
    Low.VA = 0;
    Low.Data.assign(Tab.Bytes.begin() + RecordOffset + HighBytes,
                    Tab.Bytes.end());
    Low.Size = Low.Data.size();
    T.Img.Segments.push_back(std::move(Low));
  }
}

TEST(GoMalformedRecords, DoesNotReadAFixedRecordAcrossTheAddressLimit) {
  for (uint32_t Magic : {kGo12Magic, kGo116Magic}) {
    const size_t HeaderSize = Magic == kGo12Magic ? 40 : 44;
    for (size_t HighBytes : {size_t(8), HeaderSize - 4, HeaderSize}) {
      for (BinaryFormat Format :
           {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
        SCOPED_TRACE(Magic);
        SCOPED_TRACE(HighBytes);
        SCOPED_TRACE(static_cast<unsigned>(Format));
        GoTestImage T;
        T.Img.Format = Format;
        GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
        installRecordAtAddressLimit(
            T, buildPclnTab(Magic, {Work}, kTextVA + 0x200), Magic, HighBytes);

        parseGoExceptions(T.Img);

        EXPECT_EQ(T.Img.ExceptionMetadata.GoModule.has_value(),
                  HighBytes == HeaderSize);
        const ExceptionFunction *F =
            findRecord(T.Img.ExceptionMetadata, kTextVA + 0x100);
        if (HighBytes == HeaderSize) {
          ASSERT_NE(F, nullptr);
          ASSERT_TRUE(F->Go.has_value());
          EXPECT_EQ(F->Go->Name, "main.work");
        } else {
          EXPECT_EQ(F, nullptr);
        }
      }
    }
  }
}

TEST(GoMalformedRecords, DoesNotReadPCDataSlotsAcrossTheAddressLimit) {
  for (size_t HighBytes : {size_t(44), size_t(48)}) {
    SCOPED_TRACE(HighBytes);
    GoTestImage T;
    GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
    PCValueTable UnsafePoints;
    UnsafePoints.Steps = {{-2, 16}};
    Work.PCData = {UnsafePoints};
    installRecordAtAddressLimit(
        T, buildPclnTab(kGo116Magic, {Work}, kTextVA + 0x200), kGo116Magic,
        HighBytes);

    parseGoExceptions(T.Img);

    const ExceptionFunction *F =
        findRecord(T.Img.ExceptionMetadata, kTextVA + 0x100);
    ASSERT_NE(F, nullptr);
    ASSERT_TRUE(F->Go.has_value());
    EXPECT_EQ(F->Go->UnsafePointRanges.size(), HighBytes == 48 ? 1u : 0u);
  }
}

TEST(GoMalformedRecords, DoesNotReadFuncDataSlotsAcrossTheAddressLimit) {
  for (uint32_t Magic : {kGo12Magic, kGo116Magic}) {
    const size_t ArrayOffset = Magic == kGo12Magic ? 40 : 48;
    for (size_t HighBytes : {ArrayOffset, ArrayOffset + 8}) {
      SCOPED_TRACE(Magic);
      SCOPED_TRACE(HighBytes);
      GoTestImage T;
      const va_t ArgsVA = T.addPayload(buildStackMap(1, {{1}}));
      GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
      Work.FuncData = {ArgsVA};
      // Go 1.16 needs four bytes of pointer alignment after its header. In
      // the negative case that alignment itself would wrap to address zero.
      installRecordAtAddressLimit(
          T, buildPclnTab(Magic, {Work}, kTextVA + 0x200), Magic, HighBytes);

      parseGoExceptions(T.Img);

      const ExceptionFunction *F =
          findRecord(T.Img.ExceptionMetadata, kTextVA + 0x100);
      ASSERT_NE(F, nullptr);
      ASSERT_TRUE(F->Go.has_value());
      EXPECT_EQ(F->Go->ArgsPointerMap.has_value(),
                HighBytes == ArrayOffset + 8);
      if (F->Go->ArgsPointerMap)
        EXPECT_EQ(F->Go->ArgsPointerMap->RecordVA, ArgsVA);
    }
  }
}

TEST(GoMalformedRecords, RejectsARecordDeclaringMorePCDataTablesThanExist) {
  GoTestImage T;
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  BuiltPclnTab Tab = buildPclnTab(kGo116Magic, {Work}, kTextVA + 0x200);
  // `npcdata` sizes the array the funcdata pointers sit behind, so a count
  // this large would walk the decoder off the end of the record.
  Tab.put32(Tab.RecordOffsets[0] + 32, 1000);
  T.installPclnTab(Tab.Bytes);

  parseGoExceptions(T.Img);
  EXPECT_FALSE(T.Img.ExceptionMetadata.GoModule.has_value());
}

TEST(GoMalformedRecords, RejectsARecordDeclaringMoreFuncDataThanExist) {
  GoTestImage T;
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  BuiltPclnTab Tab = buildPclnTab(kGo116Magic, {Work}, kTextVA + 0x200);
  Tab.put8(Tab.RecordOffsets[0] + 43, 200);
  T.installPclnTab(Tab.Bytes);

  parseGoExceptions(T.Img);
  EXPECT_FALSE(T.Img.ExceptionMetadata.GoModule.has_value());
}

TEST(GoMalformedRecords, RejectsAHeaderWithAnImpossiblePCQuantum) {
  GoTestImage T;
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  BuiltPclnTab Tab = buildPclnTab(kGo116Magic, {Work}, kTextVA + 0x200);
  Tab.put8(6, 3);
  T.installPclnTab(Tab.Bytes);

  EXPECT_FALSE(hasGoRuntimeMetadata(T.Img));
}

TEST(GoMalformedRecords, RejectsAHeaderWhosePointerSizeContradictsTheImage) {
  GoTestImage T;
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  BuiltPclnTab Tab = buildPclnTab(kGo116Magic, {Work}, kTextVA + 0x200);
  Tab.put8(7, 4);
  T.installPclnTab(Tab.Bytes);

  EXPECT_FALSE(hasGoRuntimeMetadata(T.Img));
}

TEST(GoMalformedRecords, RejectsAFunctionNameAddressThatWraps) {
  GoTestImage T;
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  BuiltPclnTab Tab = buildPclnTab(kGo116Magic, {Work}, kTextVA + 0x200);

  constexpr va_t WrappedNameBase = InvalidVA - 3;
  Tab.put64(24, WrappedNameBase - kPclnVA);
  Tab.put32(Tab.RecordOffsets[0] + 8, 8);
  T.installPclnTab(Tab.Bytes);

  Segment High;
  High.Name = ".high";
  High.VA = WrappedNameBase;
  High.Size = 3;
  High.Flags = SegmentFlags::Readable;
  High.Data.assign(3, 'x');
  T.Img.Segments.push_back(std::move(High));

  Segment Low;
  Low.Name = ".low";
  Low.VA = 4;
  Low.Size = 8;
  Low.Flags = SegmentFlags::Readable;
  Low.Data = {'w', 'r', 'a', 'p', 'p', 'e', 'd', 0};
  T.Img.Segments.push_back(std::move(Low));

  parseGoExceptions(T.Img);

  const ExceptionFunction *F =
      findRecord(T.Img.ExceptionMetadata, kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  EXPECT_TRUE(F->Go->Name.empty());
}

TEST(GoMalformedRecords, DoesNotReadAFunctionNameAcrossTheAddressLimit) {
  for (bool Terminated : {false, true}) {
    SCOPED_TRACE(Terminated);
    GoTestImage T;
    GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
    BuiltPclnTab Tab = buildPclnTab(kGo116Magic, {Work}, kTextVA + 0x200);
    constexpr va_t NameVA = InvalidVA - 2;
    Tab.put64(24, NameVA - kPclnVA);
    Tab.put32(Tab.RecordOffsets[0] + 8, 0);
    T.installPclnTab(Tab.Bytes);

    Segment High;
    High.VA = NameVA;
    High.Size = 3;
    High.Data = {'h', 'i', static_cast<uint8_t>(Terminated ? 0 : 'x')};
    T.Img.Segments.push_back(std::move(High));
    Segment Low;
    Low.VA = 0;
    Low.Size = 1;
    Low.Data = {0};
    T.Img.Segments.push_back(std::move(Low));

    parseGoExceptions(T.Img);

    const ExceptionFunction *F =
        findRecord(T.Img.ExceptionMetadata, kTextVA + 0x100);
    ASSERT_NE(F, nullptr);
    EXPECT_EQ(F->Go->Name, Terminated ? "hi" : "");
  }
}

TEST(GoMalformedRecords, DoesNotReadAPCVarintAcrossTheAddressLimit) {
  // Cover a valid table, a wrapped byte read, and a wrapped end cursor.
  for (unsigned Mode = 0; Mode < 3; ++Mode) {
    SCOPED_TRACE(Mode);
    GoTestImage T;
    GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
    PCValueTable UnsafePoints;
    UnsafePoints.Steps = {{-2, 16}};
    Work.PCData = {UnsafePoints};
    BuiltPclnTab Tab = buildPclnTab(kGo116Magic, {Work}, kTextVA + 0x200);
    constexpr va_t PcTabVA = InvalidVA - 3;
    Tab.put64(48, PcTabVA - kPclnVA);
    Tab.put32(Tab.PCDataArrayOffsets[0], 1);
    T.installPclnTab(Tab.Bytes);

    Segment High;
    High.VA = PcTabVA;
    High.Size = 4;
    High.Data = Mode == 0   ? std::vector<uint8_t>{0, 1, 16, 0}
                : Mode == 1 ? std::vector<uint8_t>{0, 1, 0x90, 0x80}
                            : std::vector<uint8_t>{0, 1, 0x90, 0};
    T.Img.Segments.push_back(std::move(High));
    Segment Low;
    Low.VA = 0;
    Low.Size = 4;
    Low.Data = Mode == 1 ? std::vector<uint8_t>{0x80, 0x80, 0, 0}
                         : std::vector<uint8_t>{0, 0, 0, 0};
    T.Img.Segments.push_back(std::move(Low));

    parseGoExceptions(T.Img);

    const ExceptionFunction *F =
        findRecord(T.Img.ExceptionMetadata, kTextVA + 0x100);
    ASSERT_NE(F, nullptr);
    EXPECT_EQ(F->Go->UnsafePointRanges.size(), Mode == 0 ? 1u : 0u);
    EXPECT_EQ(F->ParseStatus, Mode == 0 ? ExceptionParseStatus::Complete
                                        : ExceptionParseStatus::Partial);
  }
}

TEST(GoMalformedRecords, LeavesAnImageWithNoPclnTabAlone) {
  GoTestImage T;
  parseGoExceptions(T.Img);
  EXPECT_FALSE(T.Img.ExceptionMetadata.GoModule.has_value());
  EXPECT_TRUE(T.Img.ExceptionMetadata.Diagnostics.empty());
  EXPECT_EQ(T.Img.ExceptionMetadata.ParseStatus,
            ExceptionParseStatus::Complete);
}

} // namespace
