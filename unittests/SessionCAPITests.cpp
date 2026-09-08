//===- SessionCAPITests.cpp - public session failure contracts ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPI.h"

#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELFTypes.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace {

std::string takeString(const char *Value) {
  if (!Value)
    return {};
  std::string Text(Value);
  neverd_free_string(Value);
  return Text;
}

// A sectionless executable with one RX segment and a two-instruction body.
// Keeping both ISAs in the fixture makes decoder state observable on reload.
std::string makeNativeELF(bool AArch64) {
  using ELF = llvm::object::ELF64LE;
  using namespace llvm::ELF;
  const std::string Code =
      AArch64 ? std::string("\xe0\x00\x80\x52\xc0\x03\x5f\xd6", 8)
              : std::string("\xb8\x07\x00\x00\x00\xc3", 6);
  const size_t CodeOffset = sizeof(ELF::Ehdr) + sizeof(ELF::Phdr);
  constexpr uint64_t Base = 0x400000;
  std::string Bytes(CodeOffset + Code.size(), '\0');
  ELF::Ehdr Header{};
  std::memcpy(Header.e_ident, ElfMagic, sizeof(ElfMagic) - 1);
  Header.e_ident[EI_CLASS] = ELFCLASS64;
  Header.e_ident[EI_DATA] = ELFDATA2LSB;
  Header.e_ident[EI_VERSION] = EV_CURRENT;
  Header.e_type = ET_EXEC;
  Header.e_machine = AArch64 ? EM_AARCH64 : EM_X86_64;
  Header.e_version = EV_CURRENT;
  Header.e_entry = Base + CodeOffset;
  Header.e_phoff = sizeof(ELF::Ehdr);
  Header.e_ehsize = sizeof(ELF::Ehdr);
  Header.e_phentsize = sizeof(ELF::Phdr);
  Header.e_phnum = 1;
  Header.e_shentsize = sizeof(ELF::Shdr);
  ELF::Phdr Segment{};
  Segment.p_type = PT_LOAD;
  Segment.p_flags = PF_R | PF_X;
  Segment.p_vaddr = Base;
  Segment.p_paddr = Base;
  Segment.p_filesz = Bytes.size();
  Segment.p_memsz = Bytes.size();
  Segment.p_align = 0x1000;
  std::memcpy(Bytes.data(), &Header, sizeof(Header));
  std::memcpy(Bytes.data() + sizeof(Header), &Segment, sizeof(Segment));
  Bytes.replace(CodeOffset, Code.size(), Code);
  return Bytes;
}

class SessionCAPITest : public ::testing::Test {
protected:
  void SetUp() override {
    static std::atomic<unsigned> Sequence{0};
    const auto Stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path TemporaryRoot =
        std::filesystem::temp_directory_path();
    for (unsigned Attempt = 0; Attempt < 100; ++Attempt) {
      auto Candidate =
          TemporaryRoot / ("neverd-session-" + std::to_string(Stamp) + "-" +
                           std::to_string(Sequence.fetch_add(1)));
      std::error_code Error;
      if (std::filesystem::create_directory(Candidate, Error)) {
        Directory = std::move(Candidate);
        break;
      }
      ASSERT_FALSE(Error) << Error.message();
    }
    ASSERT_FALSE(Directory.empty());
    Session = neverd_session_create();
    ASSERT_NE(Session, nullptr);
  }

  void TearDown() override {
    neverd_session_destroy(Session);
    if (!Directory.empty()) {
      std::error_code Error;
      std::filesystem::remove_all(Directory, Error);
      EXPECT_FALSE(Error) << Error.message();
    }
  }

  std::string write(std::string_view Name, std::string_view Text) {
    const std::string Path = (Directory / Name).string();
    std::ofstream Output(Path, std::ios::binary);
    Output.write(Text.data(), static_cast<std::streamsize>(Text.size()));
    Output.close();
    EXPECT_TRUE(Output) << "cannot write fixture " << Path;
    return Path;
  }

  std::filesystem::path Directory;
  neverd_session_t Session = nullptr;
};

TEST_F(SessionCAPITest, ReloadingRenamesRestoresNamesRemovedFromTheSidecar) {
  const std::string Original = write("original.evm", "6001600055");
  ASSERT_EQ(neverd_session_load(Session, Original.c_str()), 1);
  ASSERT_EQ(neverd_rename_func(Session, "evm_entry", "reviewed_entry"), 0);
  ASSERT_EQ(takeString(neverd_func_name(Session, 0)), "reviewed_entry");

  write("original.evm.neverd-renames.json", "invalid JSON");
  EXPECT_NE(neverd_renames_load(Session), 0);
  EXPECT_EQ(takeString(neverd_func_name(Session, 0)), "reviewed_entry");

  write("original.evm.neverd-renames.json", "[]");
  ASSERT_EQ(neverd_renames_load(Session), 0);
  EXPECT_EQ(takeString(neverd_renames_json(Session)), "[]");
  EXPECT_EQ(takeString(neverd_func_name(Session, 0)), "evm_entry");
}

TEST_F(SessionCAPITest, FailedDebugReloadPreservesLoadedImageAnalysisAndEdits) {
  const std::string Original = write("original.evm", "6001600055");
  const std::string Replacement = write("replacement.evm", "6002600055");
  ASSERT_EQ(neverd_session_load(Session, Original.c_str()), 1)
      << takeString(neverd_last_error(Session));
  ASSERT_EQ(neverd_session_analyze(Session), 1)
      << takeString(neverd_last_error(Session));
  const std::string LowIR = takeString(neverd_ir_low(Session, 0));
  const std::string LLVMIR = takeString(neverd_ir_llvm(Session, 0));
  const std::string Source = takeString(neverd_decompile(Session, 0));
  ASSERT_FALSE(LowIR.empty());
  ASSERT_FALSE(LLVMIR.empty());
  ASSERT_FALSE(Source.empty());
  neverd_annotation_set(Session, 0, "retain this comment");
  ASSERT_EQ(neverd_rename_func(Session, "evm_entry", "reviewed_entry"), 0)
      << takeString(neverd_last_error(Session));
  const std::string Renames = takeString(neverd_renames_json(Session));

  for (bool PDB : {false, true}) {
    SCOPED_TRACE(PDB ? "missing PDB" : "missing MAP");
    const std::string Missing = (Directory / "absent.debug").string();
    neverd_session_set_map_path(Session, PDB ? nullptr : Missing.c_str());
    neverd_session_set_pdb_path(Session, PDB ? Missing.c_str() : nullptr);
    ASSERT_EQ(neverd_session_load(Session, Replacement.c_str()), 0);
    EXPECT_NE(takeString(neverd_last_error(Session)).find("file not found"),
              std::string::npos);
    EXPECT_EQ(neverd_session_is_loaded(Session), 1);
    EXPECT_EQ(takeString(neverd_session_file_path(Session)), Original);
    EXPECT_EQ(neverd_func_count(Session), 1);
    EXPECT_EQ(takeString(neverd_func_name(Session, 0)), "reviewed_entry");
    EXPECT_EQ(takeString(neverd_annotation_get(Session, 0)),
              "retain this comment");
    EXPECT_EQ(takeString(neverd_renames_json(Session)), Renames);
    std::array<unsigned char, 2> Bytes{};
    EXPECT_EQ(neverd_read_bytes(Session, 0, Bytes.data(), Bytes.size()), 2);
    EXPECT_EQ(Bytes[1], 1);
    EXPECT_EQ(neverd_session_analyze(Session), 1);
    EXPECT_EQ(takeString(neverd_ir_low(Session, 0)), LowIR);
    EXPECT_EQ(takeString(neverd_ir_llvm(Session, 0)), LLVMIR);
    EXPECT_EQ(takeString(neverd_decompile(Session, 0)), Source);
  }
}

TEST_F(SessionCAPITest, CachedAnalysisFailuresRetainTheirDiagnostic) {
  const std::string Path = write("requires-shanghai.evm", "5f00");
  ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1);
  ASSERT_EQ(neverd_evm_set_hardfork(Session, "london"), 1);
  ASSERT_EQ(neverd_session_analyze(Session), 0);
  const std::string Diagnostic = takeString(neverd_last_error(Session));
  ASSERT_NE(Diagnostic.find("inactive opcode"), std::string::npos);

  using TextQuery = const char *(*)(neverd_session_t, neverd_va_t);
  const std::array<TextQuery, 5> Queries{neverd_decompile, neverd_ir_low,
                                         neverd_ir_med, neverd_ir_high,
                                         neverd_ir_llvm};
  for (size_t Index = 0; Index < Queries.size(); ++Index) {
    SCOPED_TRACE(Index);
    EXPECT_TRUE(takeString(Queries[Index](Session, 0)).empty());
    EXPECT_EQ(takeString(neverd_last_error(Session)), Diagnostic);
    EXPECT_EQ(neverd_session_analyze(Session), 0);
    EXPECT_EQ(takeString(neverd_last_error(Session)), Diagnostic);
  }

  ASSERT_EQ(neverd_evm_set_hardfork(Session, "shanghai"), 1);
  EXPECT_EQ(neverd_session_analyze(Session), 1)
      << takeString(neverd_last_error(Session));
  EXPECT_FALSE(takeString(neverd_ir_low(Session, 0)).empty());
  EXPECT_TRUE(takeString(neverd_last_error(Session)).empty());
}

TEST_F(SessionCAPITest, FailedFirstLoadLeavesSessionUnloadedAndRetryable) {
  const std::string Path = write("input.evm", "6001600055");
  const std::string Missing = (Directory / "absent.map").string();
  neverd_session_set_map_path(Session, Missing.c_str());
  ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 0);
  EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  EXPECT_EQ(neverd_func_count(Session), 0);
  EXPECT_TRUE(takeString(neverd_session_file_path(Session)).empty());
  EXPECT_TRUE(takeString(neverd_ir_low(Session, 0)).empty());
  EXPECT_EQ(takeString(neverd_last_error(Session)), "no binary loaded");

  neverd_session_set_map_path(Session, nullptr);
  ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1)
      << takeString(neverd_last_error(Session));
  EXPECT_FALSE(takeString(neverd_ir_low(Session, 0)).empty());
  EXPECT_TRUE(takeString(neverd_last_error(Session)).empty());
}

TEST_F(SessionCAPITest, SuccessfulReloadReplacesAnalysisAndPerImageEdits) {
  const std::string Original = write("original.evm", "6001600055");
  const std::string Replacement = write("replacement.evm", "6002600055");
  ASSERT_EQ(neverd_session_load(Session, Original.c_str()), 1);
  const std::string OriginalIR = takeString(neverd_ir_llvm(Session, 0));
  ASSERT_FALSE(OriginalIR.empty());
  neverd_annotation_set(Session, 0, "old comment");
  ASSERT_EQ(neverd_rename_func(Session, "evm_entry", "old_name"), 0);

  ASSERT_EQ(neverd_session_load(Session, Replacement.c_str()), 1)
      << takeString(neverd_last_error(Session));
  EXPECT_EQ(takeString(neverd_session_file_path(Session)), Replacement);
  EXPECT_EQ(takeString(neverd_func_name(Session, 0)), "evm_entry");
  EXPECT_EQ(takeString(neverd_annotations_json(Session)), "[]");
  EXPECT_EQ(takeString(neverd_renames_json(Session)), "[]");
  std::array<unsigned char, 2> Bytes{};
  EXPECT_EQ(neverd_read_bytes(Session, 0, Bytes.data(), Bytes.size()), 2);
  EXPECT_EQ(Bytes[1], 2);
  const std::string ReplacementIR = takeString(neverd_ir_llvm(Session, 0));
  ASSERT_FALSE(ReplacementIR.empty());
  EXPECT_NE(ReplacementIR, OriginalIR);
  EXPECT_TRUE(takeString(neverd_last_error(Session)).empty());
}

TEST_F(SessionCAPITest, FailedNativeReloadPreservesDecoderAndDebugSelection) {
  const std::string Original = write("x86_64.elf", makeNativeELF(false));
  const std::string Replacement = write("aarch64.elf", makeNativeELF(true));
  const std::string Map =
      write("selected.map", "VMA LMA Size Align Out In Symbol\n"
                            "00400078 00400078 00000006 1 .text\n"
                            "00400078 00400078 00000006 1 reviewed_function\n");
  neverd_session_set_map_path(Session, Map.c_str());
  ASSERT_EQ(neverd_session_load(Session, Original.c_str()), 1)
      << takeString(neverd_last_error(Session));
  const neverd_va_t Entry = neverd_session_entry_addr(Session);
  const std::string Disassembly =
      takeString(neverd_disasm_json(Session, Entry, 2));
  ASSERT_NE(Disassembly.find("eax"), std::string::npos) << Disassembly;
  const std::string DebugKind =
      takeString(neverd_session_debug_info_kind(Session));
  const std::string DebugPath =
      takeString(neverd_session_debug_info_path(Session));
  ASSERT_EQ(DebugKind, "map");
  ASSERT_EQ(DebugPath, Map);
  const std::string Missing = (Directory / "absent.map").string();
  neverd_session_set_map_path(Session, Missing.c_str());
  ASSERT_EQ(neverd_session_load(Session, Replacement.c_str()), 0);
  EXPECT_EQ(takeString(neverd_session_arch_name(Session)), "x86_64");
  EXPECT_EQ(takeString(neverd_session_debug_info_kind(Session)), DebugKind);
  EXPECT_EQ(takeString(neverd_session_debug_info_path(Session)), DebugPath);
  EXPECT_EQ(takeString(neverd_disasm_json(Session, Entry, 2)), Disassembly);

  neverd_session_set_map_path(Session, nullptr);
  ASSERT_EQ(neverd_session_load(Session, Replacement.c_str()), 1)
      << takeString(neverd_last_error(Session));
  EXPECT_EQ(takeString(neverd_session_arch_name(Session)), "aarch64");
  const std::string NewDisassembly =
      takeString(neverd_disasm_json(Session, Entry, 2));
  EXPECT_NE(NewDisassembly.find("w0"), std::string::npos) << NewDisassembly;
  EXPECT_NE(NewDisassembly, Disassembly);
}

TEST_F(SessionCAPITest,
       NativeLLVMQueryUsesOriginalAddressForDebugNamedFunction) {
  const std::string Path = write("named.elf", makeNativeELF(false));
  const std::string Map =
      write("named.map", "VMA LMA Size Align Out In Symbol\n"
                         "00400078 00400078 00000006 1 .text\n"
                         "00400078 00400078 00000006 1 meaningful_name\n");
  neverd_session_set_map_path(Session, Map.c_str());
  ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1)
      << takeString(neverd_last_error(Session));
  const neverd_va_t Entry = neverd_session_entry_addr(Session);
  ASSERT_EQ(takeString(neverd_func_name(Session, 0)), "meaningful_name");

  const std::string LLVMIR = takeString(neverd_ir_llvm(Session, Entry));
  ASSERT_FALSE(LLVMIR.empty()) << takeString(neverd_last_error(Session));
  EXPECT_NE(LLVMIR.find("define "), std::string::npos) << LLVMIR;
  EXPECT_NE(LLVMIR.find("@meaningful_name("), std::string::npos) << LLVMIR;
  EXPECT_TRUE(takeString(neverd_last_error(Session)).empty());
  EXPECT_EQ(takeString(neverd_ir_llvm(Session, Entry)), LLVMIR);
}

TEST_F(SessionCAPITest, NativeLLVMQueryRejectsAnAddressThatOnlyMatchesAPrefix) {
  const std::string Path = write("anonymous.elf", makeNativeELF(false));
  ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1)
      << takeString(neverd_last_error(Session));
  const neverd_va_t Entry = neverd_session_entry_addr(Session);
  const std::string LLVMIR = takeString(neverd_ir_llvm(Session, Entry));
  ASSERT_FALSE(LLVMIR.empty()) << takeString(neverd_last_error(Session));

  EXPECT_TRUE(takeString(neverd_ir_llvm(Session, Entry >> 4)).empty());
  EXPECT_NE(takeString(neverd_last_error(Session)).find("not found"),
            std::string::npos);
  EXPECT_EQ(takeString(neverd_ir_llvm(Session, Entry)), LLVMIR);
  EXPECT_TRUE(takeString(neverd_last_error(Session)).empty());
}

TEST_F(SessionCAPITest, TargetOptionChangesInvalidatePreviouslyCachedLLVM) {
  const std::string Path = write("requires-shanghai.evm", "5f00");
  ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1);
  ASSERT_EQ(neverd_evm_set_hardfork(Session, "shanghai"), 1);
  const std::string LLVMIR = takeString(neverd_ir_llvm(Session, 0));
  ASSERT_FALSE(LLVMIR.empty()) << takeString(neverd_last_error(Session));

  ASSERT_EQ(neverd_evm_set_hardfork(Session, "london"), 1);
  EXPECT_TRUE(takeString(neverd_ir_llvm(Session, 0)).empty());
  EXPECT_NE(takeString(neverd_last_error(Session)).find("inactive opcode"),
            std::string::npos);

  ASSERT_EQ(neverd_evm_set_hardfork(Session, "shanghai"), 1);
  EXPECT_EQ(takeString(neverd_ir_llvm(Session, 0)), LLVMIR);
  EXPECT_TRUE(takeString(neverd_last_error(Session)).empty());
}

} // namespace
