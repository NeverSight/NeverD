//===- COFFARMFormatFragmentTests.cpp - ARM unwind fragment record tests -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "COFFARMFormatTestsDetail.h"
#include "neverd/backend/codegen/COFF/COFFExceptionPatch.h"

namespace {

using namespace neverd;
using namespace neverd::coff_arm_test;

TEST_F(COFFARMFormat, ARM32PackedFragmentIsRangeOnly) {
  const fs::path Source = fixture("test_patch_coff_arm.exe");
  if (!fs::exists(Source))
    GTEST_SKIP() << "ARM32 PE fixture not built (lld-link unavailable)";

  std::vector<uint8_t> Bytes = readFile(Source);
  auto Obj = createCOFFObject(Bytes);
  ASSERT_NE(Obj, nullptr);
  // Clang may encode every source function as unpacked (LLVM 22) or include a
  // packed leaf (older releases).  The mutation below replaces either form
  // with a packed fragment, so do not require the compiler to provide one.
  auto EntryOff = findUnpackedPDataEntryOffset(*Obj);
  if (!EntryOff)
    EntryOff = findPDataEntryOffsetByFlag(*Obj, 1u);
  ASSERT_TRUE(EntryOff.has_value());
  uint32_t BeginWord = readLE<uint32_t>(Bytes.data() + *EntryOff);
  va_t Addr = Obj->getImageBase() + clearThumbBit(BeginWord);
  constexpr uint32_t Length = 4;
  // The fragment still describes how to undo the parent frame from its body:
  // branch return plus one word of stack adjustment keeps that graph nonempty.
  constexpr uint32_t BodyUnwind = (1u << 13) | (1u << 22);
  writeLE<uint32_t>(Bytes.data() + *EntryOff + sizeof(uint32_t),
                    ((Length / 2) << 2) | BodyUnwind | 2u);

  fs::path Mutated = writeMutation("arm32-packed-fragment.exe", Bytes);
  auto ImgOrErr = loadBinary(Mutated);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  EXPECT_NE(std::find(ImgOrErr->KnownCodeRanges.begin(),
                      ImgOrErr->KnownCodeRanges.end(),
                      std::make_pair(Addr, Addr + Length)),
            ImgOrErr->KnownCodeRanges.end());
  EXPECT_EQ(
      std::find_if(ImgOrErr->Symbols.begin(), ImgOrErr->Symbols.end(),
                   [&](const Symbol &S) { return S.IsFunc && S.Addr == Addr; }),
      ImgOrErr->Symbols.end());
  auto Function = std::find_if(
      ImgOrErr->ExceptionMetadata.Functions.begin(),
      ImgOrErr->ExceptionMetadata.Functions.end(),
      [&](const ExceptionFunction &F) {
        return F.CodeRange.Begin == Addr &&
               F.Encoding == ExceptionEncoding::ARM32PackedFragment;
      });
  ASSERT_NE(Function, ImgOrErr->ExceptionMetadata.Functions.end());
  EXPECT_FALSE(Function->UnwindOperations.empty());
  EXPECT_EQ(Function->PrologueSize, 0u);
  expectAllFunctionRangesInsideExecutableSegments(*ImgOrErr);
}

TEST_F(COFFARMFormat, ARM64PackedFragmentIsRangeOnly) {
  const fs::path Source = fixture("test_patch_coff_a64.exe");
  if (!fs::exists(Source))
    GTEST_SKIP() << "AArch64 PE fixture not built (lld-link unavailable)";

  std::vector<uint8_t> Bytes = readFile(Source);
  auto Obj = createCOFFObject(Bytes);
  ASSERT_NE(Obj, nullptr);
  auto EntryOff = findPDataEntryOffsetByFlag(*Obj, 1u);
  ASSERT_TRUE(EntryOff.has_value());
  uint32_t BeginWord = readLE<uint32_t>(Bytes.data() + *EntryOff);
  va_t Addr = Obj->getImageBase() + BeginWord;
  constexpr uint32_t Length = 4;
  constexpr uint32_t BodyUnwind = 1u << 23; // one 16-byte frame allocation
  writeLE<uint32_t>(Bytes.data() + *EntryOff + sizeof(uint32_t),
                    ((Length / 4) << 2) | BodyUnwind | 2u);

  fs::path Mutated = writeMutation("arm64-packed-fragment.exe", Bytes);
  auto ImgOrErr = loadBinary(Mutated);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  EXPECT_NE(std::find(ImgOrErr->KnownCodeRanges.begin(),
                      ImgOrErr->KnownCodeRanges.end(),
                      std::make_pair(Addr, Addr + Length)),
            ImgOrErr->KnownCodeRanges.end());
  EXPECT_EQ(
      std::find_if(ImgOrErr->Symbols.begin(), ImgOrErr->Symbols.end(),
                   [&](const Symbol &S) { return S.IsFunc && S.Addr == Addr; }),
      ImgOrErr->Symbols.end());
  auto Function = std::find_if(
      ImgOrErr->ExceptionMetadata.Functions.begin(),
      ImgOrErr->ExceptionMetadata.Functions.end(),
      [&](const ExceptionFunction &F) {
        return F.CodeRange.Begin == Addr &&
               F.Encoding == ExceptionEncoding::ARM64PackedFragment;
      });
  ASSERT_NE(Function, ImgOrErr->ExceptionMetadata.Functions.end());
  EXPECT_FALSE(Function->UnwindOperations.empty());
  EXPECT_EQ(Function->PrologueSize, 0u);
  expectAllFunctionRangesInsideExecutableSegments(*ImgOrErr);
}

TEST_F(COFFARMFormat, ARM32UnpackedFragmentIsRangeOnly) {
  const fs::path Source = fixture("test_patch_coff_arm.exe");
  if (!fs::exists(Source))
    GTEST_SKIP() << "ARM32 PE fixture not built (lld-link unavailable)";

  auto BaselineOrErr = loadBinary(Source);
  ASSERT_TRUE(static_cast<bool>(BaselineOrErr))
      << llvm::toString(BaselineOrErr.takeError());
  std::vector<uint8_t> Bytes = readFile(Source);
  auto Obj = createCOFFObject(Bytes);
  ASSERT_NE(Obj, nullptr);
  auto EntryOff = findUnpackedPDataEntryOffset(*Obj);
  ASSERT_TRUE(EntryOff.has_value());
  auto XDataOff = xdataFileOffset(Bytes, *Obj, *BaselineOrErr, *EntryOff);
  ASSERT_TRUE(XDataOff.has_value());
  uint32_t BeginWord = readLE<uint32_t>(Bytes.data() + *EntryOff);
  va_t Addr = Obj->getImageBase() + clearThumbBit(BeginWord);
  uint32_t Header = readLE<uint32_t>(Bytes.data() + *XDataOff);
  uint32_t Length = (Header & 0x3ffffu) * 2;
  ASSERT_NE(Length, 0u);
  writeLE<uint32_t>(Bytes.data() + *XDataOff, Header | (1u << 22));

  fs::path Mutated = writeMutation("arm32-unpacked-fragment.exe", Bytes);
  auto ImgOrErr = loadBinary(Mutated);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  EXPECT_NE(std::find(ImgOrErr->KnownCodeRanges.begin(),
                      ImgOrErr->KnownCodeRanges.end(),
                      std::make_pair(Addr, Addr + Length)),
            ImgOrErr->KnownCodeRanges.end());
  EXPECT_EQ(
      std::find_if(ImgOrErr->Symbols.begin(), ImgOrErr->Symbols.end(),
                   [&](const Symbol &S) { return S.IsFunc && S.Addr == Addr; }),
      ImgOrErr->Symbols.end());
  auto Function = std::find_if(
      ImgOrErr->ExceptionMetadata.Functions.begin(),
      ImgOrErr->ExceptionMetadata.Functions.end(),
      [&](const ExceptionFunction &F) {
        return F.CodeRange.Begin == Addr &&
               F.Encoding == ExceptionEncoding::ARM32Unpacked;
      });
  ASSERT_NE(Function, ImgOrErr->ExceptionMetadata.Functions.end());
  EXPECT_EQ(Function->Kind, RuntimeFunctionKind::Fragment);
  EXPECT_FALSE(Function->UnwindOperations.empty());
  EXPECT_EQ(Function->PrologueSize, 0u);
  expectAllFunctionRangesInsideExecutableSegments(*ImgOrErr);
}

TEST_F(COFFARMFormat, ARM64CorrespondingXDataBitIsNotFragment) {
  const fs::path Source = fixture("test_patch_coff_a64.exe");
  if (!fs::exists(Source))
    GTEST_SKIP() << "AArch64 PE fixture not built (lld-link unavailable)";

  auto BaselineOrErr = loadBinary(Source);
  ASSERT_TRUE(static_cast<bool>(BaselineOrErr))
      << llvm::toString(BaselineOrErr.takeError());
  std::vector<uint8_t> Bytes = readFile(Source);
  auto Obj = createCOFFObject(Bytes);
  ASSERT_NE(Obj, nullptr);
  auto EntryOff = findUnpackedPDataEntryOffset(*Obj);
  ASSERT_TRUE(EntryOff.has_value());
  auto XDataOff = xdataFileOffset(Bytes, *Obj, *BaselineOrErr, *EntryOff);
  ASSERT_TRUE(XDataOff.has_value());
  uint32_t BeginWord = readLE<uint32_t>(Bytes.data() + *EntryOff);
  va_t Addr = Obj->getImageBase() + BeginWord;
  constexpr uint32_t Length = 4;
  writeLE<uint32_t>(Bytes.data() + *XDataOff, (Length / 4) | (1u << 22));

  fs::path Mutated = writeMutation("arm64-not-fragment.exe", Bytes);
  auto ImgOrErr = loadBinary(Mutated);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  EXPECT_NE(std::find(ImgOrErr->KnownCodeRanges.begin(),
                      ImgOrErr->KnownCodeRanges.end(),
                      std::make_pair(Addr, Addr + Length)),
            ImgOrErr->KnownCodeRanges.end());
  EXPECT_NE(std::find_if(ImgOrErr->Symbols.begin(), ImgOrErr->Symbols.end(),
                         [&](const Symbol &S) {
                           return S.IsFunc && S.Addr == Addr &&
                                  S.Size == Length;
                         }),
            ImgOrErr->Symbols.end());
  expectAllFunctionRangesInsideExecutableSegments(*ImgOrErr);
}

TEST_F(COFFARMFormat, ARM64UnpackedEndChainedIsRangeOnly) {
  const fs::path Source = fixture("test_patch_coff_a64.exe");
  if (!fs::exists(Source))
    GTEST_SKIP() << "AArch64 PE fixture not built (lld-link unavailable)";

  auto BaselineOrErr = loadBinary(Source);
  ASSERT_TRUE(static_cast<bool>(BaselineOrErr))
      << llvm::toString(BaselineOrErr.takeError());
  std::vector<uint8_t> Bytes = readFile(Source);
  auto Obj = createCOFFObject(Bytes);
  ASSERT_NE(Obj, nullptr);
  auto EntryOff = findUnpackedPDataEntryOffset(*Obj);
  ASSERT_TRUE(EntryOff.has_value());
  auto XDataOff = xdataFileOffset(Bytes, *Obj, *BaselineOrErr, *EntryOff);
  ASSERT_TRUE(XDataOff.has_value());

  uint32_t BeginWord = readLE<uint32_t>(Bytes.data() + *EntryOff);
  va_t Addr = Obj->getImageBase() + BeginWord;
  uint32_t Header = readLE<uint32_t>(Bytes.data() + *XDataOff);
  uint32_t Length = (Header & 0x3ffffu) * 4;
  ASSERT_NE(Length, 0u);

  // One real shrink-wrapped prologue operation precedes `end_c`; the second
  // allocation belongs to the parent/phantom scope.  E=1 makes the single
  // epilogue begin at code index zero, and one code word holds the sequence.
  writeLE<uint32_t>(Bytes.data() + *XDataOff,
                    (Length / 4) | (1u << 21) | (1u << 27));
  uint8_t *Codes = Bytes.data() + *XDataOff + sizeof(uint32_t);
  Codes[0] = 0x01; // alloc_s: a real four-byte prologue instruction
  Codes[1] = 0xE5; // end_c: remaining operations are from the parent scope
  Codes[2] = 0x01; // parent/phantom alloc_s
  Codes[3] = 0xE4; // end

  fs::path Mutated = writeMutation("arm64-unpacked-end-c-fragment.exe", Bytes);
  auto ImgOrErr = loadBinary(Mutated);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  EXPECT_NE(std::find(ImgOrErr->KnownCodeRanges.begin(),
                      ImgOrErr->KnownCodeRanges.end(),
                      std::make_pair(Addr, Addr + Length)),
            ImgOrErr->KnownCodeRanges.end());
  EXPECT_EQ(
      std::find_if(ImgOrErr->Symbols.begin(), ImgOrErr->Symbols.end(),
                   [&](const Symbol &S) { return S.IsFunc && S.Addr == Addr; }),
      ImgOrErr->Symbols.end());

  auto Function = std::find_if(
      ImgOrErr->ExceptionMetadata.Functions.begin(),
      ImgOrErr->ExceptionMetadata.Functions.end(),
      [&](const ExceptionFunction &F) {
        return F.CodeRange.Begin == Addr &&
               F.Encoding == ExceptionEncoding::ARM64Unpacked;
      });
  ASSERT_NE(Function, ImgOrErr->ExceptionMetadata.Functions.end());
  EXPECT_EQ(Function->Kind, RuntimeFunctionKind::Fragment);
  EXPECT_NE(std::find_if(Function->UnwindOperations.begin(),
                         Function->UnwindOperations.end(),
                         [](const UnwindOperation &Op) {
                           return Op.Kind == UnwindOperationKind::EndChained;
                         }),
            Function->UnwindOperations.end());
  EXPECT_EQ(Function->PrologueSize, 4u);
  expectAllFunctionRangesInsideExecutableSegments(*ImgOrErr);

  // A rewrite can be planned for a primary record in the same image, but an
  // independently addressable chained range makes the ARM host association
  // ambiguous.  Pin the loader-to-patch contract so `end_c` fragments cannot
  // silently cross that fail-closed boundary.
  ExceptionFunction Primary;
  Primary.CodeRange = {ImgOrErr->Base + 0x100000,
                       ImgOrErr->Base + 0x100020};
  Primary.Kind = RuntimeFunctionKind::Primary;
  Primary.Encoding = ExceptionEncoding::ARM64Unpacked;
  Primary.ParseStatus = ExceptionParseStatus::Complete;

  MedFunc Func;
  Func.Entry = Primary.CodeRange.Begin;
  Func.Name = "arm64_primary_for_fragment_barrier";
  MedBlock Block;
  Block.Id = 0;
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = Func.Entry;
  Block.Ops.push_back(std::move(Return));
  Func.Blocks.push_back(std::move(Block));
  Func.ExceptionMetadata = Primary;

  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit({Func}, Context, "arm64-fragment-barrier",
                                      Arch::AArch64, {}, nullptr,
                                      BinaryFormat::COFF);
  ASSERT_NE(Module, nullptr);
  ImgOrErr->ExceptionMetadata.Functions.push_back(std::move(Primary));
  ImgOrErr->ExceptionMetadata.rebuildIndex();

  auto Plan = planCOFFExceptionPatch(*Module, *ImgOrErr, Arch::AArch64);
  ASSERT_FALSE(static_cast<bool>(Plan));
  EXPECT_EQ(llvm::toString(Plan.takeError()),
            "coff exception patch: ARM image contains independently "
            "addressable function fragments; their host association is not "
            "provable for rewrite");
}

} // namespace
