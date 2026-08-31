//===- PatchFormatSectionTests.cpp - Renamed code section patch tests -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "PatchFormatTestsDetail.h"
#include "gtest/gtest.h"

namespace {

using namespace neverd;
using namespace neverd::patch_format_test;

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
  ASSERT_EQ(R.exitCode, 0) << "inplace patch with -text-section=.text failed: "
                           << R.err;
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
  auto R = exec(ndBin(),
                {"patch", "-mode=inplace", "-text-section=.nd_no_such_section",
                 "-o", Out.string(), Elf.string()});
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
  for (auto It =
           std::search(Buf.begin(), Buf.end(), Needle.begin(), Needle.end());
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
  ASSERT_TRUE(
      renameCodeSection(Elf, Renamed, section_names::elf::Text, ".vmp0"))
      << "could not rename .text in test ELF";
  auto Out = tmpFile("patched_ip");
  auto R = exec(ndBin(), {"patch", "-mode=inplace", "-o", Out.string(),
                          Renamed.string()});
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
  ASSERT_TRUE(
      renameCodeSection(Elf, Renamed, section_names::elf::Text, ".vmp0"));
  auto Out = tmpFile("patched_ovr");
  auto R = exec(ndBin(), {"patch", "-mode=inplace", "-text-section=.vmp0", "-o",
                          Out.string(), Renamed.string()});
  ASSERT_EQ(R.exitCode, 0)
      << "inplace patch with explicit -text-section=.vmp0 failed: " << R.err;
  EXPECT_TRUE(fs::exists(Out));
}

#ifdef __APPLE__
// These smoke fixtures select the implemented DWARF registration path.
// Compact-unwind regeneration remains a separately tracked capability.
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
  auto CR = exec("clang", {"-O1", "-Wl,-no_compact_unwind", "-o", Bin.string(),
                           Src.string()});
  if (CR.exitCode != 0)
    GTEST_SKIP() << "host clang could not build Mach-O test: " << CR.err;

  auto Renamed = tmpFile("m_renamed");
  ASSERT_TRUE(
      renameCodeSection(Bin, Renamed, section_names::macho::Text, "__vmp0"))
      << "could not rename __text in host Mach-O";
  auto Out = tmpFile("m_patched");
  auto R = exec(ndBin(), {"patch", "-o", Out.string(), Renamed.string()});
  ASSERT_EQ(R.exitCode, 0) << "MachO section patch failed: " << R.err;
  EXPECT_FALSE(R.contains(", 0 trampolines"))
      << "fallback should have located the renamed code section:\n"
      << R.out;
}

// Explicit --text-section override on a host Mach-O whose "__text" was renamed:
// MachOPatcher::parseLayout must honour the forced name and install
// trampolines. (The override-matching branch; the renamed section stays inside
// __TEXT, which is the only place the loader can recover correct symbol VAs
// from — its entry point and LC_FUNCTION_STARTS bases are both keyed off the
// __TEXT segment.)
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
  auto CR = exec("clang", {"-O1", "-Wl,-no_compact_unwind", "-o", Bin.string(),
                           Src.string()});
  if (CR.exitCode != 0)
    GTEST_SKIP() << "host clang could not build Mach-O test: " << CR.err;

  auto Renamed = tmpFile("mo_renamed");
  ASSERT_TRUE(
      renameCodeSection(Bin, Renamed, section_names::macho::Text, "__vmp0"))
      << "could not rename __text in host Mach-O";
  auto Out = tmpFile("mo_patched");
  auto R = exec(ndBin(), {"patch", "-text-section=__vmp0", "-o", Out.string(),
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
// Keep these fixtures free of unwind metadata: renaming __TEXT also renames an
// __eh_frame owner, which is intentionally not a dyld-registrable
// __TEXT,__eh_frame and exercises a separate fail-closed contract.
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
  auto CR = exec("clang", {"-O1", "-fno-asynchronous-unwind-tables",
                           "-fno-unwind-tables", "-Wl,-no_compact_unwind", "-o",
                           Bin.string(), Src.string()});
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
  auto CR = exec("clang", {"-O1", "-fno-asynchronous-unwind-tables",
                           "-fno-unwind-tables", "-Wl,-no_compact_unwind", "-o",
                           Bin.string(), Src.string()});
  if (CR.exitCode != 0)
    GTEST_SKIP() << "host clang could not build Mach-O test: " << CR.err;

  auto Renamed = tmpFile("ma_renamed");
  ASSERT_TRUE(renameAllOccurrences(Bin, Renamed, "__TEXT", "__PCK0"))
      << "could not rename __TEXT segment in host Mach-O";
  auto Out = tmpFile("ma_patched");
  auto R = exec(ndBin(), {"patch", "-o", Out.string(), Renamed.string()});
  ASSERT_EQ(R.exitCode, 0) << "patch of renamed __TEXT segment (auto) failed: "
                           << R.err;
  EXPECT_FALSE(R.contains(", 0 trampolines"))
      << "exec-segment fallback should have located the code section:\n"
      << R.out;
}
#endif

} // namespace
