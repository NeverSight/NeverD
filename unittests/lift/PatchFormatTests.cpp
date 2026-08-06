//===- PatchFormatTests.cpp - Cross-format ELF patch tests ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Tests verifying that the patch pipeline works for linked ELF executables
/// across x86-64, AArch64, and ARM32 architectures.
///
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"

#include "neverd/Object/SectionNames.h"
#include "neverd/Object/PELayout.h"
#include "neverd/Support/BinaryLoading.h"
#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/codegen/COFF/COFFPatch.h"
#include "neverd/decode/Decoder.h"

#include "llvm/Support/Error.h"

#include <algorithm>
#include <cctype>
#include <iterator>

using namespace neverd;

//===----------------------------------------------------------------------===//
// x86-64 ELF patch
//===----------------------------------------------------------------------===//

class PatchELF_X64 : public NeverDLiftTest {};

TEST_F(PatchELF_X64, SwitchPatchSucceeds) {
  auto Elf = fs::path(TEST_OBJ_DIR) / "test_patch_switch_elf";
  if (!fs::exists(Elf))
    GTEST_SKIP() << "ELF executable not built (ld.lld not available)";
  auto R = patchBinary(Elf);
  ASSERT_EQ(R.exitCode, 0) << "ELF patch failed: " << R.err;
  auto PatchedFile = tmpFile("patched");
  ASSERT_TRUE(fs::exists(PatchedFile)) << "Patched binary not created";
  EXPECT_GT(fs::file_size(PatchedFile), 0u) << "Patched binary is empty";
}

TEST_F(PatchELF_X64, AllStagesThenPatch) {
  auto Elf = fs::path(TEST_OBJ_DIR) / "test_patch_switch_elf";
  if (!fs::exists(Elf))
    GTEST_SKIP() << "ELF executable not built";
  verifyAllStages(Elf);
  auto R = patchBinary(Elf);
  ASSERT_EQ(R.exitCode, 0) << "Patch after lift failed: " << R.err;
}

TEST_F(PatchELF_X64, PatchedBinaryIsValidELF) {
  auto Elf = fs::path(TEST_OBJ_DIR) / "test_patch_switch_elf";
  if (!fs::exists(Elf))
    GTEST_SKIP() << "ELF executable not built";
  auto R = patchBinary(Elf);
  ASSERT_EQ(R.exitCode, 0) << R.err;
  auto PatchedFile = tmpFile("patched");
  ASSERT_TRUE(fs::exists(PatchedFile));

  auto VerifyR = liftToLowIR(PatchedFile);
  EXPECT_EQ(VerifyR.exitCode, 0)
      << "Patched binary should be liftable: " << VerifyR.err;
}

//===----------------------------------------------------------------------===//
// AArch64 ELF patch
//===----------------------------------------------------------------------===//

class PatchELF_AArch64 : public NeverDLiftTest {};

TEST_F(PatchELF_AArch64, SwitchPatchSucceeds) {
  auto Elf = fs::path(TEST_OBJ_DIR) / "test_patch_switch_a64_elf";
  if (!fs::exists(Elf))
    GTEST_SKIP() << "AArch64 ELF executable not built";
  auto R = patchBinary(Elf);
  ASSERT_EQ(R.exitCode, 0) << "AArch64 ELF patch failed: " << R.err;
  auto PatchedFile = tmpFile("patched");
  ASSERT_TRUE(fs::exists(PatchedFile)) << "Patched binary not created";
  EXPECT_GT(fs::file_size(PatchedFile), 0u);
}

TEST_F(PatchELF_AArch64, AllStagesThenPatch) {
  auto Elf = fs::path(TEST_OBJ_DIR) / "test_patch_switch_a64_elf";
  if (!fs::exists(Elf))
    GTEST_SKIP() << "AArch64 ELF executable not built";
  verifyAllStages(Elf);
  auto R = patchBinary(Elf);
  ASSERT_EQ(R.exitCode, 0) << "Patch after lift failed: " << R.err;
}

//===----------------------------------------------------------------------===//
// ARM32 ELF patch
//===----------------------------------------------------------------------===//

class PatchELF_ARM32 : public NeverDLiftTest {};

TEST_F(PatchELF_ARM32, SwitchPatchSucceeds) {
  auto Elf = fs::path(TEST_OBJ_DIR) / "test_patch_switch_arm_elf";
  if (!fs::exists(Elf))
    GTEST_SKIP() << "ARM32 ELF executable not built";
  auto R = patchBinary(Elf);
  ASSERT_EQ(R.exitCode, 0) << "ARM32 ELF patch failed: " << R.err;
  auto PatchedFile = tmpFile("patched");
  ASSERT_TRUE(fs::exists(PatchedFile)) << "Patched binary not created";
  EXPECT_GT(fs::file_size(PatchedFile), 0u);
}

TEST_F(PatchELF_ARM32, AllStagesThenPatch) {
  auto Elf = fs::path(TEST_OBJ_DIR) / "test_patch_switch_arm_elf";
  if (!fs::exists(Elf))
    GTEST_SKIP() << "ARM32 ELF executable not built";
  verifyAllStages(Elf);
  auto R = patchBinary(Elf);
  ASSERT_EQ(R.exitCode, 0) << "Patch after lift failed: " << R.err;
}

//===----------------------------------------------------------------------===//
// x86-64 COFF/PE patch
//===----------------------------------------------------------------------===//

class PatchCOFF_X64 : public NeverDLiftTest {};

TEST_F(PatchCOFF_X64, SwitchPatchSucceeds) {
  auto PE = fs::path(TEST_OBJ_DIR) / "test_patch_coff.exe";
  if (!fs::exists(PE))
    GTEST_SKIP() << "COFF executable not built (lld-link not available)";
  auto R = patchBinary(PE);
  ASSERT_EQ(R.exitCode, 0) << "COFF patch failed: " << R.err;
  auto PatchedFile = tmpFile("patched");
  ASSERT_TRUE(fs::exists(PatchedFile)) << "Patched binary not created";
  EXPECT_GT(fs::file_size(PatchedFile), 0u) << "Patched binary is empty";
}

TEST_F(PatchCOFF_X64, AllStagesThenPatch) {
  auto PE = fs::path(TEST_OBJ_DIR) / "test_patch_coff.exe";
  if (!fs::exists(PE))
    GTEST_SKIP() << "COFF executable not built";
  verifyAllStages(PE);
  auto R = patchBinary(PE);
  ASSERT_EQ(R.exitCode, 0) << "Patch after lift failed: " << R.err;
}

TEST_F(PatchCOFF_X64, PatchedBinaryIsValidPE) {
  auto PE = fs::path(TEST_OBJ_DIR) / "test_patch_coff.exe";
  if (!fs::exists(PE))
    GTEST_SKIP() << "COFF executable not built";
  auto R = patchBinary(PE);
  ASSERT_EQ(R.exitCode, 0) << R.err;
  auto PatchedFile = tmpFile("patched");
  ASSERT_TRUE(fs::exists(PatchedFile));

  auto VerifyR = liftToLowIR(PatchedFile);
  EXPECT_EQ(VerifyR.exitCode, 0)
      << "Patched PE should be liftable: " << VerifyR.err;
}

//===----------------------------------------------------------------------===//
// ARM32 COFF/PE patch
//===----------------------------------------------------------------------===//

class PatchCOFF_ARM32 : public NeverDLiftTest {};

TEST_F(PatchCOFF_ARM32, SectionPatchInstallsTrampolineAndRelifts) {
  auto PE = fs::path(TEST_OBJ_DIR) / "test_patch_coff_arm.exe";
  if (!fs::exists(PE))
    GTEST_SKIP() << "ARM32 PE executable not built (lld-link not available)";

  auto R = patchBinary(PE);
  ASSERT_EQ(R.exitCode, 0) << "ARM32 PE patch failed: " << R.err;
  EXPECT_FALSE(R.contains(", 0 trampolines)")) << R.out;

  auto PatchedFile = tmpFile("patched");
  ASSERT_TRUE(fs::exists(PatchedFile));
  EXPECT_GT(fs::file_size(PatchedFile), fs::file_size(PE));

  auto ImgOrErr = loadBinary(PatchedFile);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  EXPECT_EQ(ImgOrErr->Format, BinaryFormat::COFF);
  EXPECT_EQ(ImgOrErr->Arch, Arch::ARM);
  EXPECT_EQ(ImgOrErr->Mode, InstructionMode::Thumb);

  auto Relift = liftToLowIR(PatchedFile);
  ASSERT_EQ(Relift.exitCode, 0) << Relift.err;
  std::string Output = Relift.out + "\n" + Relift.err;
  std::transform(Output.begin(), Output.end(), Output.begin(),
                 [](unsigned char C) { return std::tolower(C); });
  EXPECT_EQ(Output.find("unlifted"), std::string::npos) << Output;
}

TEST_F(PatchCOFF_ARM32, EntryTrampolineBranchesIntoAppendedExecSection) {
  auto PE = fs::path(TEST_OBJ_DIR) / "test_patch_coff_arm.exe";
  if (!fs::exists(PE))
    GTEST_SKIP() << "ARM32 PE executable not built (lld-link not available)";

  auto R = patchBinary(PE);
  ASSERT_EQ(R.exitCode, 0) << R.err;
  auto ImgOrErr = loadBinary(tmpFile("patched"));
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  const BinaryImage &Img = *ImgOrErr;

  auto ExportIt =
      std::find_if(Img.Exports.begin(), Img.Exports.end(),
                   [](const Export &E) { return E.Name == "pe_leaf"; });
  ASSERT_NE(ExportIt, Img.Exports.end());
  const Segment *OrigText = Img.getSegmentFor(ExportIt->Addr);
  ASSERT_NE(OrigText, nullptr);
  ASSERT_TRUE(OrigText->isExecutable());
  uint64_t EntryOff = ExportIt->Addr - OrigText->VA;
  ASSERT_LE(EntryOff, OrigText->Data.size());
  ASSERT_GE(OrigText->Data.size() - EntryOff, 4u);

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::ARM, InstructionMode::Thumb));
  DecodedInsn Insn{};
  ASSERT_EQ(
      Dec.decodeOne(OrigText->Data.data() + EntryOff, 4, ExportIt->Addr, Insn),
      4);
  ASSERT_STREQ(Insn.Raw->mnemonic, "b.w");
  ASSERT_NE(Insn.Raw->detail, nullptr);
  ASSERT_EQ(Insn.Raw->detail->arm.op_count, 1u);
  ASSERT_EQ(Insn.Raw->detail->arm.operands[0].type, ARM_OP_IMM);
  va_t Target = Insn.Raw->detail->arm.operands[0].imm;

  auto NewText = std::find_if(
      Img.Sections.begin(), Img.Sections.end(),
      [](const Section &S) { return S.Name == kNdTextSection && S.isExecutable(); });
  ASSERT_NE(NewText, Img.Sections.end());
  EXPECT_TRUE(NewText->contains(Target))
      << "Thumb b.w target 0x" << std::hex << Target
      << " is outside appended executable section";
}

TEST_F(PatchCOFF_ARM32, TwoByteExportHasAdjacentBoundary) {
  auto PE = fs::path(TEST_OBJ_DIR) / "test_patch_coff_arm.exe";
  if (!fs::exists(PE))
    GTEST_SKIP() << "ARM32 PE executable not built (lld-link not available)";

  auto ImgOrErr = loadBinary(PE);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  const auto &Exports = ImgOrErr->Exports;
  auto Tiny = std::find_if(Exports.begin(), Exports.end(),
                           [](const Export &E) { return E.Name == "pe_tiny"; });
  auto After =
      std::find_if(Exports.begin(), Exports.end(),
                   [](const Export &E) { return E.Name == "pe_after_tiny"; });
  ASSERT_NE(Tiny, Exports.end());
  ASSERT_NE(After, Exports.end());
  EXPECT_EQ(After->Addr - Tiny->Addr, 2u);
}

TEST_F(PatchCOFF_ARM32, InstallerSkipsTwoByteFunctionWithoutTouchingSentinel) {
  auto PE = fs::path(TEST_OBJ_DIR) / "test_patch_coff_arm.exe";
  if (!fs::exists(PE))
    GTEST_SKIP() << "ARM32 PE executable not built (lld-link not available)";

  auto ImgOrErr = loadBinary(PE);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  const BinaryImage &Img = *ImgOrErr;
  auto Tiny = std::find_if(Img.Exports.begin(), Img.Exports.end(),
                           [](const Export &E) { return E.Name == "pe_tiny"; });
  auto After =
      std::find_if(Img.Exports.begin(), Img.Exports.end(),
                   [](const Export &E) { return E.Name == "pe_after_tiny"; });
  ASSERT_NE(Tiny, Img.Exports.end());
  ASSERT_NE(After, Img.Exports.end());
  ASSERT_EQ(After->Addr - Tiny->Addr, 2u);

  std::ifstream In(PE, std::ios::binary);
  ASSERT_TRUE(In.good());
  std::vector<uint8_t> Bytes(std::istreambuf_iterator<char>(In), {});
  std::vector<uint8_t> Before = Bytes;
  auto Headers = locatePEHeaders(Bytes.data(), Bytes.size());
  ASSERT_TRUE(Headers.valid());
  PESectionFields Text;
  ASSERT_TRUE(findPESection(Headers, section_names::coff::Text, Text));

  COFFPatcher Patcher;
  Patcher.setImageContext(&Img);
  uint64_t AppendedVA = Patcher.plannedExecSegmentVA(Bytes, Arch::ARM);
  ASSERT_NE(AppendedVA, 0u);

  std::map<std::string, uint64_t> OnlyTiny{{"pe_tiny", AppendedVA}};
  uint64_t TextSize = Text.VirtualSize ? Text.VirtualSize : Text.SizeOfRawData;
  size_t Count = BinaryPatcher::installTrampolines(
      Bytes, OnlyTiny, Text.VirtualAddress, TextSize, Text.PointerToRawData,
      Img.Base, Img.Arch, Img.Mode, &Img.Symbols, &Img.KnownCodeRanges,
      &Img.Exports);
  EXPECT_EQ(Count, 0u);

  ASSERT_GE(Tiny->Addr, Img.Base + Text.VirtualAddress);
  uint64_t TinyDelta = Tiny->Addr - Img.Base - Text.VirtualAddress;
  ASSERT_LE(TinyDelta, InvalidVA - Text.PointerToRawData);
  uint64_t TinyOff = Text.PointerToRawData + TinyDelta;
  constexpr size_t TinyAndSentinelSize = 6;
  ASSERT_LE(TinyOff, Bytes.size());
  ASSERT_GE(Bytes.size() - TinyOff, TinyAndSentinelSize);
  EXPECT_TRUE(std::equal(Before.begin() + TinyOff,
                         Before.begin() + TinyOff + TinyAndSentinelSize,
                         Bytes.begin() + TinyOff));
}

//===----------------------------------------------------------------------===//
// AArch64 COFF/PE patch
//===----------------------------------------------------------------------===//

class PatchCOFF_AArch64 : public NeverDLiftTest {};

TEST_F(PatchCOFF_AArch64, SectionPatchInstallsTrampolineAndRelifts) {
  auto PE = fs::path(TEST_OBJ_DIR) / "test_patch_coff_a64.exe";
  if (!fs::exists(PE))
    GTEST_SKIP() << "AArch64 PE executable not built (lld-link not available)";

  auto R = patchBinary(PE);
  ASSERT_EQ(R.exitCode, 0) << "AArch64 PE patch failed: " << R.err;
  EXPECT_FALSE(R.contains(", 0 trampolines)")) << R.out;

  auto PatchedFile = tmpFile("patched");
  ASSERT_TRUE(fs::exists(PatchedFile));
  EXPECT_GT(fs::file_size(PatchedFile), fs::file_size(PE));

  auto ImgOrErr = loadBinary(PatchedFile);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  EXPECT_EQ(ImgOrErr->Format, BinaryFormat::COFF);
  EXPECT_EQ(ImgOrErr->Arch, Arch::AArch64);
  EXPECT_EQ(ImgOrErr->Mode, InstructionMode::Default);

  auto Relift = liftToLowIR(PatchedFile);
  ASSERT_EQ(Relift.exitCode, 0) << Relift.err;
  std::string Output = Relift.out + "\n" + Relift.err;
  std::transform(Output.begin(), Output.end(), Output.begin(),
                 [](unsigned char C) { return std::tolower(C); });
  EXPECT_EQ(Output.find("unlifted"), std::string::npos) << Output;
}

TEST_F(PatchCOFF_AArch64, EntryTrampolineBranchesIntoAppendedExecSection) {
  auto PE = fs::path(TEST_OBJ_DIR) / "test_patch_coff_a64.exe";
  if (!fs::exists(PE))
    GTEST_SKIP() << "AArch64 PE executable not built (lld-link not available)";

  auto R = patchBinary(PE);
  ASSERT_EQ(R.exitCode, 0) << R.err;
  auto ImgOrErr = loadBinary(tmpFile("patched"));
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  const BinaryImage &Img = *ImgOrErr;

  auto ExportIt =
      std::find_if(Img.Exports.begin(), Img.Exports.end(),
                   [](const Export &E) { return E.Name == "pe_leaf"; });
  ASSERT_NE(ExportIt, Img.Exports.end());
  const Segment *OrigText = Img.getSegmentFor(ExportIt->Addr);
  ASSERT_NE(OrigText, nullptr);
  ASSERT_TRUE(OrigText->isExecutable());
  uint64_t EntryOff = ExportIt->Addr - OrigText->VA;
  ASSERT_LE(EntryOff, OrigText->Data.size());
  ASSERT_GE(OrigText->Data.size() - EntryOff, 4u);

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::AArch64));
  DecodedInsn Insn{};
  ASSERT_EQ(
      Dec.decodeOne(OrigText->Data.data() + EntryOff, 4, ExportIt->Addr, Insn),
      4);
  ASSERT_STREQ(Insn.Raw->mnemonic, "b");
  ASSERT_NE(Insn.Raw->detail, nullptr);
  ASSERT_EQ(Insn.Raw->detail->aarch64.op_count, 1u);
  ASSERT_EQ(Insn.Raw->detail->aarch64.operands[0].type, AARCH64_OP_IMM);
  va_t Target = Insn.Raw->detail->aarch64.operands[0].imm;

  auto NewText = std::find_if(
      Img.Sections.begin(), Img.Sections.end(),
      [](const Section &S) { return S.Name == kNdTextSection && S.isExecutable(); });
  ASSERT_NE(NewText, Img.Sections.end());
  EXPECT_TRUE(NewText->contains(Target))
      << "AArch64 b target 0x" << std::hex << Target
      << " is outside appended executable section";
}

//===----------------------------------------------------------------------===//
// Cross-function call patch tests (xcall binaries)
//===----------------------------------------------------------------------===//

class PatchXCall_X64 : public NeverDLiftTest {};

TEST_F(PatchXCall_X64, PatchSucceeds) {
  auto Elf = fs::path(TEST_OBJ_DIR) / "test_patch_xcall_elf";
  if (!fs::exists(Elf))
    GTEST_SKIP() << "xcall ELF executable not built";
  auto R = patchBinary(Elf);
  ASSERT_EQ(R.exitCode, 0) << "xcall patch failed: " << R.err;
  auto PatchedFile = tmpFile("patched");
  ASSERT_TRUE(fs::exists(PatchedFile)) << "Patched binary not created";
  EXPECT_GT(fs::file_size(PatchedFile), 0u);
}

TEST_F(PatchXCall_X64, AllStagesThenPatch) {
  auto Elf = fs::path(TEST_OBJ_DIR) / "test_patch_xcall_elf";
  if (!fs::exists(Elf))
    GTEST_SKIP() << "xcall ELF executable not built";
  verifyAllStages(Elf);
  auto R = patchBinary(Elf);
  ASSERT_EQ(R.exitCode, 0) << "Patch after lift failed: " << R.err;
}

TEST_F(PatchXCall_X64, PatchedBinaryReliftable) {
  auto Elf = fs::path(TEST_OBJ_DIR) / "test_patch_xcall_elf";
  if (!fs::exists(Elf))
    GTEST_SKIP() << "xcall ELF executable not built";
  auto R = patchBinary(Elf);
  ASSERT_EQ(R.exitCode, 0) << R.err;
  auto PatchedFile = tmpFile("patched");
  ASSERT_TRUE(fs::exists(PatchedFile));
  auto VerifyR = liftToLowIR(PatchedFile);
  EXPECT_EQ(VerifyR.exitCode, 0) << "Patched xcall binary should be liftable";
}

class PatchXCall_AArch64 : public NeverDLiftTest {};

TEST_F(PatchXCall_AArch64, PatchSucceeds) {
  auto Elf = fs::path(TEST_OBJ_DIR) / "test_patch_xcall_a64_elf";
  if (!fs::exists(Elf))
    GTEST_SKIP() << "AArch64 xcall ELF not built";
  auto R = patchBinary(Elf);
  ASSERT_EQ(R.exitCode, 0) << "AArch64 xcall patch failed: " << R.err;
  auto PatchedFile = tmpFile("patched");
  EXPECT_TRUE(fs::exists(PatchedFile));
}

TEST_F(PatchXCall_AArch64, AllStagesThenPatch) {
  auto Elf = fs::path(TEST_OBJ_DIR) / "test_patch_xcall_a64_elf";
  if (!fs::exists(Elf))
    GTEST_SKIP() << "AArch64 xcall ELF not built";
  verifyAllStages(Elf);
  auto R = patchBinary(Elf);
  ASSERT_EQ(R.exitCode, 0) << "Patch after lift failed: " << R.err;
}

class PatchXCall_ARM32 : public NeverDLiftTest {};

TEST_F(PatchXCall_ARM32, PatchSucceeds) {
  auto Elf = fs::path(TEST_OBJ_DIR) / "test_patch_xcall_arm_elf";
  if (!fs::exists(Elf))
    GTEST_SKIP() << "ARM32 xcall ELF not built";
  auto R = patchBinary(Elf);
  ASSERT_EQ(R.exitCode, 0) << "ARM32 xcall patch failed: " << R.err;
  auto PatchedFile = tmpFile("patched");
  EXPECT_TRUE(fs::exists(PatchedFile));
}

TEST_F(PatchXCall_ARM32, AllStagesThenPatch) {
  auto Elf = fs::path(TEST_OBJ_DIR) / "test_patch_xcall_arm_elf";
  if (!fs::exists(Elf))
    GTEST_SKIP() << "ARM32 xcall ELF not built";
  verifyAllStages(Elf);
  auto R = patchBinary(Elf);
  ASSERT_EQ(R.exitCode, 0) << "Patch after lift failed: " << R.err;
}

//===----------------------------------------------------------------------===//
// --text-section override (target a renamed / packed code section)
//===----------------------------------------------------------------------===//

class PatchTextSectionOverride : public NeverDLiftTest {};

// Passing the canonical name explicitly must take the override code path in
// parseTextSection and produce a working in-place patch (same as the default).
TEST_F(PatchTextSectionOverride, ExplicitCanonicalNameInplaceSucceeds) {
  auto Elf = fs::path(TEST_OBJ_DIR) / "test_patch_switch_elf";
  if (!fs::exists(Elf))
    GTEST_SKIP() << "ELF executable not built";
  auto Out = tmpFile("patched_override");
  auto R = exec(ndBin(), {"patch", "-mode=inplace", "-text-section=.text", "-o",
                          Out.string(), Elf.string()});
  ASSERT_EQ(R.exitCode, 0)
      << "inplace patch with -text-section=.text failed: " << R.err;
  EXPECT_TRUE(fs::exists(Out)) << "Patched binary not created";
  EXPECT_GT(fs::file_size(Out), 0u);
  EXPECT_TRUE(R.contains("trampoline")) << "Expected trampolines to be written";
}

// A bogus override must NOT be fatal: parseTextSection falls through to the
// format default (".text"), then to the flag-based fallback.  The patch still
// succeeds, proving the override is a hint layered on top of the existing
// detection rather than a hard requirement.
TEST_F(PatchTextSectionOverride, UnknownNameFallsBackAndSucceeds) {
  auto Elf = fs::path(TEST_OBJ_DIR) / "test_patch_switch_elf";
  if (!fs::exists(Elf))
    GTEST_SKIP() << "ELF executable not built";
  auto Out = tmpFile("patched_override_bogus");
  auto R = exec(ndBin(), {"patch", "-mode=inplace",
                          "-text-section=.nd_no_such_section", "-o",
                          Out.string(), Elf.string()});
  ASSERT_EQ(R.exitCode, 0)
      << "inplace patch should fall back when override is absent: " << R.err;
  EXPECT_TRUE(fs::exists(Out)) << "Patched binary not created";
}

//===----------------------------------------------------------------------===//
// Renamed/packed code section — auto-recovery without --text-section
//===----------------------------------------------------------------------===//

namespace {
// Simulate a packer/protector that renamed the code section: rewrite the first
// NUL-terminated occurrence of \p OldName to \p NewName (which must be the same
// length) in the file at \p Src, writing \p Dst.  Only the name string changes;
// section flags, segment permissions and the symbol table stay intact — exactly
// the residue a real packer leaves, which is what the flag-based fallback keys
// off of.  Returns false if the name is absent or the lengths differ.
bool renameCodeSection(const fs::path &Src, const fs::path &Dst,
                       const std::string &OldName, const std::string &NewName) {
  if (OldName.size() != NewName.size())
    return false;
  std::ifstream In(Src, std::ios::binary);
  if (!In)
    return false;
  std::vector<char> Buf((std::istreambuf_iterator<char>(In)),
                        std::istreambuf_iterator<char>());
  std::string Needle = OldName;
  Needle.push_back('\0');
  auto It = std::search(Buf.begin(), Buf.end(), Needle.begin(), Needle.end());
  if (It == Buf.end())
    return false;
  std::copy(NewName.begin(), NewName.end(), It);
  std::ofstream Out(Dst, std::ios::binary);
  if (!Out)
    return false;
  Out.write(Buf.data(), static_cast<std::streamsize>(Buf.size()));
  return Out.good();
}

// Rename *every* NUL-terminated occurrence of \p OldName to \p NewName (same
// length).  Used to rename a whole Mach-O segment consistently — its segment
// command's segname plus the segname field of each section it owns — to mimic a
// packer that relocates code out of the canonically-named __TEXT segment.  A
// lowercase section name such as "__text" is case-distinct from the "__TEXT"
// segment name and therefore stays intact (still targetable by --text-section).
bool renameAllOccurrences(const fs::path &Src, const fs::path &Dst,
                          const std::string &OldName,
                          const std::string &NewName) {
  if (OldName.size() != NewName.size())
    return false;
  std::ifstream In(Src, std::ios::binary);
  if (!In)
    return false;
  std::vector<char> Buf((std::istreambuf_iterator<char>(In)),
                        std::istreambuf_iterator<char>());
  std::string Needle = OldName;
  Needle.push_back('\0');
  bool Any = false;
  for (auto It = std::search(Buf.begin(), Buf.end(), Needle.begin(),
                             Needle.end());
       It != Buf.end();
       It = std::search(It + 1, Buf.end(), Needle.begin(), Needle.end())) {
    std::copy(NewName.begin(), NewName.end(), It);
    Any = true;
  }
  if (!Any)
    return false;
  std::ofstream Out(Dst, std::ios::binary);
  if (!Out)
    return false;
  Out.write(Buf.data(), static_cast<std::streamsize>(Buf.size()));
  return Out.good();
}
} // namespace

class RenamedSectionPatch : public NeverDLiftTest {};

// In-place hardening of a binary whose ".text" was renamed must succeed via the
// flag-based fallback in parseTextSection — no --text-section needed.
TEST_F(RenamedSectionPatch, ElfInplaceAutoRecovers) {
  auto Elf = fs::path(TEST_OBJ_DIR) / "test_patch_switch_elf";
  if (!fs::exists(Elf))
    GTEST_SKIP() << "ELF executable not built";
  auto Renamed = tmpFile("renamed.elf");
  ASSERT_TRUE(renameCodeSection(Elf, Renamed, section_names::elf::Text, ".vmp0"))
      << "could not rename .text in test ELF";
  auto Out = tmpFile("patched_ip");
  auto R = exec(ndBin(),
                {"patch", "-mode=inplace", "-o", Out.string(), Renamed.string()});
  ASSERT_EQ(R.exitCode, 0)
      << "inplace patch must auto-recover the renamed code section: " << R.err;
  EXPECT_TRUE(fs::exists(Out));
}

// The same renamed binary can be targeted explicitly with --text-section.
TEST_F(RenamedSectionPatch, ElfInplaceExplicitOverride) {
  auto Elf = fs::path(TEST_OBJ_DIR) / "test_patch_switch_elf";
  if (!fs::exists(Elf))
    GTEST_SKIP() << "ELF executable not built";
  auto Renamed = tmpFile("renamed.elf");
  ASSERT_TRUE(renameCodeSection(Elf, Renamed, section_names::elf::Text, ".vmp0"));
  auto Out = tmpFile("patched_ovr");
  auto R = exec(ndBin(), {"patch", "-mode=inplace", "-text-section=.vmp0", "-o",
                          Out.string(), Renamed.string()});
  ASSERT_EQ(R.exitCode, 0)
      << "inplace patch with explicit -text-section=.vmp0 failed: " << R.err;
  EXPECT_TRUE(fs::exists(Out));
}

#ifdef __APPLE__
// Section-mode patching of a host Mach-O whose "__text" was renamed must still
// install trampolines, exercising the executable-segment fallback added to
// MachOPatcher::parseLayout.  Without it the patch "succeeds" but writes zero
// trampolines (the redirection is silently dropped).
TEST_F(RenamedSectionPatch, MachOSectionModeAutoRecovers) {
  auto Src = tmpFile("m.c");
  {
    std::ofstream O(Src);
    O << "int a(int x){return x+1;}\n"
         "int b(int x){return x*2;}\n"
         "int c(int x){return x-3;}\n"
         "int main(){return a(b(c(7)));}\n";
  }
  auto Bin = tmpFile("m");
  auto CR = exec("clang", {"-O1", "-o", Bin.string(), Src.string()});
  if (CR.exitCode != 0)
    GTEST_SKIP() << "host clang could not build Mach-O test: " << CR.err;

  auto Renamed = tmpFile("m_renamed");
  ASSERT_TRUE(renameCodeSection(Bin, Renamed, section_names::macho::Text, "__vmp0"))
      << "could not rename __text in host Mach-O";
  auto Out = tmpFile("m_patched");
  auto R = exec(ndBin(), {"patch", "-o", Out.string(), Renamed.string()});
  ASSERT_EQ(R.exitCode, 0) << "MachO section patch failed: " << R.err;
  EXPECT_FALSE(R.contains(", 0 trampolines"))
      << "fallback should have located the renamed code section:\n"
      << R.out;
}

// Explicit --text-section override on a host Mach-O whose "__text" was renamed:
// MachOPatcher::parseLayout must honour the forced name and install trampolines.
// (The override-matching branch; the renamed section stays inside __TEXT, which
// is the only place the loader can recover correct symbol VAs from — its entry
// point and LC_FUNCTION_STARTS bases are both keyed off the __TEXT segment.)
TEST_F(RenamedSectionPatch, MachOSectionModeExplicitOverride) {
  auto Src = tmpFile("mo.c");
  {
    std::ofstream O(Src);
    O << "int a(int x){return x+1;}\n"
         "int b(int x){return x*2;}\n"
         "int c(int x){return x-3;}\n"
         "int main(){return a(b(c(7)));}\n";
  }
  auto Bin = tmpFile("mo");
  auto CR = exec("clang", {"-O1", "-o", Bin.string(), Src.string()});
  if (CR.exitCode != 0)
    GTEST_SKIP() << "host clang could not build Mach-O test: " << CR.err;

  auto Renamed = tmpFile("mo_renamed");
  ASSERT_TRUE(renameCodeSection(Bin, Renamed, section_names::macho::Text, "__vmp0"))
      << "could not rename __text in host Mach-O";
  auto Out = tmpFile("mo_patched");
  auto R = exec(ndBin(),
                {"patch", "-text-section=__vmp0", "-o", Out.string(),
                 Renamed.string()});
  ASSERT_EQ(R.exitCode, 0)
      << "MachO section patch with -text-section=__vmp0 failed: " << R.err;
  EXPECT_FALSE(R.contains(", 0 trampolines"))
      << "explicit override should have located the renamed code section:\n"
      << R.out;
}

// A protector that renames the entire __TEXT *segment* (not just the __text
// section) relocates the code into a segment the loader cannot recognise by
// name.  MachOLoader recovers the image base from whichever segment maps file
// offset 0, so the entry point, LC_FUNCTION_STARTS deltas and export VAs stay
// correct; the code section is then reachable cross-segment via --text-section.
// (Before the image-base recovery this produced zero trampolines because every
// symbol VA was wrong upstream — Entry came back as 0x340 instead of
// 0x100000340.)
TEST_F(RenamedSectionPatch, MachORenamedTextSegmentExplicitOverride) {
  auto Src = tmpFile("ms.c");
  {
    std::ofstream O(Src);
    O << "int a(int x){return x+1;}\n"
         "int b(int x){return x*2;}\n"
         "int c(int x){return x-3;}\n"
         "int main(){return a(b(c(7)));}\n";
  }
  auto Bin = tmpFile("ms");
  auto CR = exec("clang", {"-O1", "-o", Bin.string(), Src.string()});
  if (CR.exitCode != 0)
    GTEST_SKIP() << "host clang could not build Mach-O test: " << CR.err;

  auto Renamed = tmpFile("ms_renamed");
  ASSERT_TRUE(renameAllOccurrences(Bin, Renamed, "__TEXT", "__PCK0"))
      << "could not rename __TEXT segment in host Mach-O";
  auto Out = tmpFile("ms_patched");
  auto R = exec(ndBin(), {"patch", "-text-section=__text", "-o", Out.string(),
                          Renamed.string()});
  ASSERT_EQ(R.exitCode, 0)
      << "patch of renamed __TEXT segment (override) failed: " << R.err;
  EXPECT_FALSE(R.contains(", 0 trampolines"))
      << "cross-segment override should have located the code section:\n"
      << R.out;
}

// The same renamed-__TEXT-segment binary with no --text-section: the loader's
// image-base recovery combined with the patcher's largest-executable-segment
// fallback must still install trampolines.
TEST_F(RenamedSectionPatch, MachORenamedTextSegmentAutoRecovers) {
  auto Src = tmpFile("ma.c");
  {
    std::ofstream O(Src);
    O << "int a(int x){return x+1;}\n"
         "int b(int x){return x*2;}\n"
         "int c(int x){return x-3;}\n"
         "int main(){return a(b(c(7)));}\n";
  }
  auto Bin = tmpFile("ma");
  auto CR = exec("clang", {"-O1", "-o", Bin.string(), Src.string()});
  if (CR.exitCode != 0)
    GTEST_SKIP() << "host clang could not build Mach-O test: " << CR.err;

  auto Renamed = tmpFile("ma_renamed");
  ASSERT_TRUE(renameAllOccurrences(Bin, Renamed, "__TEXT", "__PCK0"))
      << "could not rename __TEXT segment in host Mach-O";
  auto Out = tmpFile("ma_patched");
  auto R = exec(ndBin(), {"patch", "-o", Out.string(), Renamed.string()});
  ASSERT_EQ(R.exitCode, 0)
      << "patch of renamed __TEXT segment (auto) failed: " << R.err;
  EXPECT_FALSE(R.contains(", 0 trampolines"))
      << "exec-segment fallback should have located the code section:\n"
      << R.out;
}
#endif
