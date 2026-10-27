//===- BinaryDiffSDKTests.cpp - Native comparison integration -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Real loader/decoder/C API fixtures. Test binaries are authored machine
/// code; comparisons never execute them or contact a service.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPI.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Object/ELFTypes.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace {
namespace sve_compare_fixture {
#define NEVERD_SVE_COMPARE_VALUE(Type, Name, Value) constexpr Type Name = Value;
#include "BinaryDiffSVEPrefixFixtures.def"
} // namespace sve_compare_fixture

std::string take(const char *S) {
  const std::string Out = S ? S : "";
  neverd_free_string(S);
  return Out;
}
llvm::json::Value parsed(const char *S) {
  auto V = llvm::json::parse(take(S));
  if (!V) {
    ADD_FAILURE() << llvm::toString(V.takeError());
    return llvm::json::Object{};
  }
  return std::move(*V);
}
std::string render(const llvm::json::Value &V) {
  std::string S;
  llvm::raw_string_ostream OS(S);
  OS << V;
  return S;
}

// Inspection cancellation is cooperative: a request may race the last poll or
// the operation may finish before this thread observes it. Only a successfully
// completed attempt can be retried, with refresh, within one shared deadline.
// Errors and other stop states return immediately so the caller's assertions
// still fail. The test must obtain an actual cancelled result before
// proceeding.
template <typename Inspect>
std::string cancelRunningInspection(neverd_comparison_t C,
                                    llvm::StringRef Phase,
                                    Inspect InspectFresh) {
  const auto Deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  std::string Text;
  do {
    std::promise<void> Start;
    auto Ready = Start.get_future();
    auto Future = std::async(std::launch::async, [&] {
      Ready.wait();
      return take(InspectFresh());
    });
    Start.set_value();
    while (Future.wait_for(std::chrono::milliseconds(0)) !=
               std::future_status::ready &&
           std::chrono::steady_clock::now() < Deadline) {
      const auto Status = parsed(neverd_comparison_status_json(C));
      const auto *O = Status.getAsObject();
      if (O && O->getBoolean("running").value_or(false) &&
          O->getString("phase") == Phase)
        neverd_comparison_cancel(C);
      std::this_thread::yield();
    }
    if (Future.wait_for(std::chrono::seconds(10)) !=
        std::future_status::ready) {
      ADD_FAILURE() << "Inspection did not stop: "
                    << take(neverd_comparison_status_json(C));
      return {};
    }
    Text = Future.get();
    auto Result = llvm::json::parse(Text);
    if (!Result) {
      ADD_FAILURE() << llvm::toString(Result.takeError()) << "\n" << Text;
      return Text;
    }
    const auto *O = Result->getAsObject();
    if (!O || O->getString("state") != "complete")
      return Text;
  } while (std::chrono::steady_clock::now() < Deadline);
  ADD_FAILURE() << "No in-flight cancellation was acknowledged: " << Text
                << "\nStatus: " << take(neverd_comparison_status_json(C))
                << "\nError: " << take(neverd_comparison_last_error(C));
  return Text;
}

class BinaryDiffSDK : public ::testing::Test {
protected:
  std::filesystem::path Directory;
  std::vector<neverd_session_t> Sessions;
  std::vector<neverd_comparison_t> Comparisons;
  void SetUp() override {
    llvm::SmallString<128> Path;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-bindiff", Path));
    Directory = Path.c_str();
  }
  void TearDown() override {
    for (auto C : Comparisons)
      neverd_comparison_destroy(C);
    for (auto S : Sessions)
      if (S)
        neverd_session_destroy(S);
    std::error_code Error;
    std::filesystem::remove_all(Directory, Error);
  }
  neverd_session_t input(llvm::ArrayRef<uint8_t> Bytes, uint64_t Base = 0x1000,
                         const char *Processor = "x86_64") {
    const auto Path = Directory / (std::to_string(Sessions.size()) + ".raw");
    std::ofstream File(Path, std::ios::binary);
    File.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
    File.close();
    auto S = neverd_session_create();
    Sessions.push_back(S);
    const auto Options = render(llvm::json::Object{{"loader", "binary"},
                                                   {"processor", Processor},
                                                   {"base", Base},
                                                   {"platform", "sysv"}});
    EXPECT_EQ(neverd_session_set_load_options(S, Options.c_str()), 0);
    EXPECT_EQ(neverd_session_load(S, Path.string().c_str()), 1)
        << take(neverd_last_error(S));
    return S;
  }
  neverd_comparison_t compare(neverd_session_t A, neverd_session_t B,
                              const char *Options = "{\"discover\":false}") {
    auto C = neverd_comparison_create();
    Comparisons.push_back(C);
    EXPECT_EQ(neverd_comparison_run(C, A, B, Options, nullptr, nullptr), 1)
        << take(neverd_comparison_last_error(C));
    return C;
  }
  void checkBranchProgress(llvm::StringRef Hex, unsigned Version = 4,
                           unsigned GuardCount = 2,
                           llvm::StringRef ProofKind = {});
  static std::vector<uint8_t> body(uint8_t Immediate = 7) {
    return {0xb8, Immediate, 0, 0, 0, 0x83, 0xc0, 3, 0xc3};
  }
  static std::vector<uint8_t> words(std::initializer_list<uint32_t> Words) {
    std::vector<uint8_t> Bytes;
    for (const uint32_t Word : Words)
      for (unsigned Shift = 0; Shift < 32; Shift += 8)
        Bytes.push_back(static_cast<uint8_t>(Word >> Shift));
    return Bytes;
  }
};

TEST_F(BinaryDiffSDK, ByteOverlaysBindDataOnlyChangesAndReopenedReports) {
  const std::array<uint8_t, 24> Bytes{0xb8, 7, 0, 0, 0, 0xc3};
  auto A = input(Bytes);
  auto B = input(Bytes);
  const auto Before = take(neverd_comparison_export_json(compare(A, B)));
  auto Original = llvm::json::parse(Before);
  ASSERT_TRUE(bool(Original));
  const auto *OriginalIdentity = Original->getAsObject()->getObject("primary");
  ASSERT_TRUE(OriginalIdentity);
  neverd_byte_patch_options_v1 Patch{};
  Patch.struct_size = sizeof(Patch);
  Patch.address = 0x1010;
  Patch.kind = "bytes";
  Patch.bytes = "2a";
  ASSERT_FALSE(take(neverd_byte_patch_apply(A, &Patch)).empty())
      << take(neverd_last_error(A));
  const auto ChangedComparison = compare(A, B);
  const auto After = take(neverd_comparison_export_json(ChangedComparison));
  auto Changed = llvm::json::parse(After);
  ASSERT_TRUE(bool(Changed));
  const auto *ChangedIdentity = Changed->getAsObject()->getObject("primary");
  ASSERT_TRUE(ChangedIdentity);
  EXPECT_EQ(OriginalIdentity->getString("sha256"),
            ChangedIdentity->getString("sha256"));
  EXPECT_NE(OriginalIdentity->getString("analysis"),
            ChangedIdentity->getString("analysis"));
  EXPECT_NE(OriginalIdentity->getString("revision"),
            ChangedIdentity->getString("revision"));
  EXPECT_EQ(render(*Original->getAsObject()->get("functions")),
            render(*Changed->getAsObject()->get("functions")));
  EXPECT_EQ(neverd_comparison_restore_json(ChangedComparison, Before.c_str()),
            0);
  EXPECT_FALSE(take(neverd_comparison_last_error(ChangedComparison)).empty());
  EXPECT_EQ(take(neverd_comparison_export_json(ChangedComparison)), After);

  // An unpacked sidecar must bind the same effective bytes after reload.
  ASSERT_EQ(neverd_byte_patches_save(A), 0) << take(neverd_last_error(A));
  auto Replayed = neverd_session_create();
  Sessions.push_back(Replayed);
  ASSERT_EQ(
      neverd_session_set_load_options(
          Replayed,
          R"({"loader":"binary","processor":"x86_64","platform":"sysv","base":4096})"),
      0);
  ASSERT_EQ(
      neverd_session_load(Replayed, (Directory / "0.raw").string().c_str()), 1)
      << take(neverd_last_error(Replayed));
  const auto Reopened = compare(Replayed, B);
  EXPECT_EQ(neverd_comparison_restore_json(Reopened, After.c_str()), 1)
      << take(neverd_comparison_last_error(Reopened));

  ASSERT_EQ(neverd_byte_patch_revert(A, Patch.address, 1), 0);
  const auto Restored = compare(A, B);
  EXPECT_EQ(neverd_comparison_restore_json(Restored, Before.c_str()), 1)
      << take(neverd_comparison_last_error(Restored));
}

TEST_F(BinaryDiffSDK, FunctionNameProvenanceSurvivesReorderingAndRenames) {
  for (const bool AArch64 : {false, true})
    for (const unsigned Symbols : {0u, 1u, 2u}) {
      SCOPED_TRACE(AArch64);
      SCOPED_TRACE(Symbols);
      const auto Image = [&](bool Reordered) {
        auto Bytes = AArch64 ? words({Reordered ? 0x94000008u : 0x94000004u,
                                      Reordered ? 0x94000003u : 0x94000007u,
                                      0xd65f03c0, 0xd503201f})
                             : std::vector<uint8_t>{
                                   0xe8, uint8_t(Reordered ? 27 : 11), 0, 0, 0,
                                   0xe8, uint8_t(Reordered ? 6 : 22),  0, 0, 0,
                                   0xc3};
        Bytes.resize(48, AArch64 ? 0 : 0xcc);
        for (unsigned I = 0; I < 2; ++I) {
          const auto Which = Reordered ? 1 - I : I;
          const auto Code =
              AArch64 ? words({Which ? 0x52800260u : 0x528000e0u, 0xd65f03c0})
                      : body(Which ? 19 : 7);
          std::copy(Code.begin(), Code.end(), Bytes.begin() + 16 * (I + 1));
        }
        // In the stripped variant, only the entry and direct call targets
        // identify starts. Other variants retain duplicate local symbols or
        // genuine symbols whose spelling happens to resemble generated labels.
        using ELF = llvm::object::ELF64LE;
        using namespace llvm::ELF;
        constexpr size_t CodeOffset = 0x100;
        std::vector<uint8_t> File(CodeOffset + Bytes.size(), 0);
        ELF::Ehdr Header{};
        std::memcpy(Header.e_ident, ElfMagic, sizeof(ElfMagic) - 1);
        Header.e_ident[EI_CLASS] = ELFCLASS64;
        Header.e_ident[EI_DATA] = ELFDATA2LSB;
        Header.e_ident[EI_VERSION] = EV_CURRENT;
        Header.e_type = ET_EXEC;
        Header.e_machine = AArch64 ? EM_AARCH64 : EM_X86_64;
        Header.e_version = EV_CURRENT;
        Header.e_entry = 0x400000 + CodeOffset;
        Header.e_phoff = sizeof(ELF::Ehdr);
        Header.e_ehsize = sizeof(ELF::Ehdr);
        Header.e_phentsize = sizeof(ELF::Phdr);
        Header.e_phnum = 1;
        Header.e_shentsize = sizeof(ELF::Shdr);
        ELF::Phdr Segment{};
        Segment.p_type = PT_LOAD;
        Segment.p_flags = PF_R | PF_X;
        Segment.p_vaddr = 0x400000;
        Segment.p_paddr = 0x400000;
        Segment.p_filesz = File.size();
        Segment.p_memsz = File.size();
        Segment.p_align = 0x1000;
        std::memcpy(File.data(), &Header, sizeof(Header));
        std::memcpy(File.data() + sizeof(Header), &Segment, sizeof(Segment));
        std::copy(Bytes.begin(), Bytes.end(), File.begin() + CodeOffset);
        if (Symbols) {
          std::array<ELF::Sym, 4> Names{};
          std::string Strings(1, '\0');
          const auto AddName = [&](llvm::StringRef Name) {
            const auto Offset = Strings.size();
            Strings.append(Name.data(), Name.size());
            Strings.push_back('\0');
            return Offset;
          };
          for (unsigned I = 0; I < 2; ++I) {
            const auto Which = Reordered ? 1 - I : I;
            auto &Sym = Names[I + 1];
            Sym.st_name = AddName(Symbols == 1 ? "helper"
                                  : Which      ? "sub_400120"
                                               : "sub_400110");
            Sym.setBindingAndType(STB_LOCAL, STT_FUNC);
            Sym.st_shndx = 1;
            Sym.st_value = 0x400110 + 16 * I;
            Sym.st_size = AArch64 ? 8 : 6;
          }
          Names[3].st_name = AddName("entry");
          Names[3].setBindingAndType(STB_GLOBAL, STT_FUNC);
          Names[3].st_shndx = 1;
          Names[3].st_value = Header.e_entry;
          Names[3].st_size = AArch64 ? 12 : 11;
          std::array<ELF::Shdr, 4> Sections{};
          Sections[1].sh_type = SHT_PROGBITS;
          Sections[1].sh_flags = SHF_ALLOC | SHF_EXECINSTR;
          Sections[1].sh_addr = Header.e_entry;
          Sections[1].sh_offset = CodeOffset;
          Sections[1].sh_size = Bytes.size();
          Sections[1].sh_addralign = 16;
          Sections[2].sh_type = SHT_SYMTAB;
          Sections[2].sh_offset = File.size();
          Sections[2].sh_size = sizeof(Names);
          Sections[2].sh_link = 3;
          Sections[2].sh_info = 3;
          Sections[2].sh_addralign = 8;
          Sections[2].sh_entsize = sizeof(ELF::Sym);
          const auto Append = [&](const void *Data, size_t Size) {
            const auto *Begin = static_cast<const uint8_t *>(Data);
            File.insert(File.end(), Begin, Begin + Size);
          };
          Append(Names.data(), sizeof(Names));
          Sections[3].sh_type = SHT_STRTAB;
          Sections[3].sh_offset = File.size();
          Sections[3].sh_size = Strings.size();
          Sections[3].sh_addralign = 1;
          Append(Strings.data(), Strings.size());
          File.resize((File.size() + 7) & ~size_t(7), 0);
          Header.e_shoff = File.size();
          Header.e_shnum = Sections.size();
          Append(Sections.data(), sizeof(Sections));
          std::memcpy(File.data(), &Header, sizeof(Header));
        }
        return File;
      };
      const auto Load = [&](bool Reordered) {
        const auto Bytes = Image(Reordered);
        const auto Path =
            Directory / (std::to_string(Sessions.size()) + ".elf");
        std::ofstream File(Path, std::ios::binary);
        File.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
        File.close();
        auto S = neverd_session_create();
        Sessions.push_back(S);
        EXPECT_EQ(neverd_session_load(S, Path.string().c_str()), 1)
            << take(neverd_last_error(S));
        return S;
      };
      const auto A = Load(false), B = Load(true);
      const auto Before = parsed(
          neverd_comparison_export_json(compare(A, B, R"({"discover":true})")));
      ASSERT_TRUE(Before.getAsObject());
      const auto *Matches = Before.getAsObject()->getArray("matches");
      ASSERT_TRUE(Matches);
      ASSERT_EQ(Matches->size(), 3u) << render(Before);
      const std::map<std::string, std::string> Expected{
          {"0x400100", "0x400100"},
          {"0x400110", "0x400120"},
          {"0x400120", "0x400110"}};
      for (const auto &Value : *Matches) {
        const auto *Row = Value.getAsObject();
        ASSERT_TRUE(Row && Row->getString("primary"));
        const auto Found = Expected.find(Row->getString("primary")->str());
        ASSERT_NE(Found, Expected.end());
        EXPECT_EQ(Row->getString("secondary"), Found->second);
        if (!Symbols || (Symbols == 1 && Found->first != "0x400100"))
          EXPECT_NE(Row->getString("step"), "symbol_name");
        else
          EXPECT_EQ(Row->getString("step"), "symbol_name");
      }
      // An analyst can give different functions the same display annotation;
      // that must not replace the extraction's names or manufacture an anchor.
      ASSERT_EQ(neverd_rename_addr(A, 0x400110, "analyst_label"), 0);
      ASSERT_EQ(neverd_rename_addr(B, 0x400110, "analyst_label"), 0);
      const auto Renamed = compare(A, B, R"({"discover":true})");
      const auto After = parsed(neverd_comparison_export_json(Renamed));
      ASSERT_TRUE(After.getAsObject());
      for (const auto *Key : {"primary", "secondary", "functions", "matches"}) {
        ASSERT_TRUE(Before.getAsObject()->get(Key));
        ASSERT_TRUE(After.getAsObject()->get(Key));
        EXPECT_EQ(*Before.getAsObject()->get(Key),
                  *After.getAsObject()->get(Key))
            << Key;
      }
      ASSERT_EQ(neverd_comparison_restore_json(Renamed, render(Before).c_str()),
                1)
          << take(neverd_comparison_last_error(Renamed));
      for (const auto *Side : {"primary", "secondary"}) {
        const auto Query = render(
            llvm::json::Object{{"side", Side}, {"search", "analyst_label"}});
        const auto Page =
            parsed(neverd_comparison_functions_json(Renamed, Query.c_str()));
        const auto *Rows = Page.getAsObject()->getArray("items");
        ASSERT_TRUE(Rows);
        ASSERT_EQ(Rows->size(), 1u);
        const auto *Row = Rows->front().getAsObject();
        EXPECT_EQ(Row->getString("address"), "0x400110");
        EXPECT_EQ(Row->getString("name"), "analyst_label");
        EXPECT_EQ(Row->getString("evidence_name"),
                  Symbols == 1 ? "helper"
                  : Symbols == 2 && llvm::StringRef(Side) == "secondary"
                      ? "sub_400120"
                      : "sub_400110");
      }
    }
}

TEST_F(BinaryDiffSDK, AnalysisSectionsBindDataOutsideTheFunctionInventory) {
  auto A = input(body());
  auto B = input(body());
  const auto Before = take(neverd_comparison_export_json(compare(A, B)));
  auto Original = llvm::json::parse(Before);
  ASSERT_TRUE(bool(Original));
  const uint8_t Data[] = {42, 3, 4};
  ASSERT_EQ(neverd_analysis_section_add(
                A, R"({"schema":"neverd.analysis-section.v1","name":".extra",
                       "address":"0x2000","permissions":"rw"})",
                Data, sizeof(Data)),
            1)
      << take(neverd_last_error(A));
  auto C = compare(A, B);
  const auto After = take(neverd_comparison_export_json(C));
  auto Changed = llvm::json::parse(After);
  ASSERT_TRUE(bool(Changed));
  const auto &Old = *Original->getAsObject()->getObject("primary");
  const auto &New = *Changed->getAsObject()->getObject("primary");
  EXPECT_EQ(Old.getString("sha256"), New.getString("sha256"));
  EXPECT_NE(Old.getString("analysis"), New.getString("analysis"));
  EXPECT_NE(Old.getString("revision"), New.getString("revision"));
  EXPECT_EQ(render(*Original->getAsObject()->get("functions")),
            render(*Changed->getAsObject()->get("functions")));
  EXPECT_EQ(neverd_comparison_restore_json(C, Before.c_str()), 0);
  EXPECT_EQ(take(neverd_comparison_export_json(C)), After);

  auto L = neverd_lineage_create();
  ASSERT_TRUE(L);
  const auto Document =
      render(llvm::json::Object{{"schema", "neverd.bindiff.lineage"},
                                {"schema_version", 1},
                                {"reports", llvm::json::Array{Before, After}}});
  EXPECT_EQ(
      neverd_lineage_load_json(L, Document.c_str(), nullptr, nullptr, nullptr),
      1)
      << take(neverd_lineage_last_error(L));
  const auto Summary = parsed(neverd_lineage_summary_json(L));
  ASSERT_TRUE(Summary.getAsObject());
  EXPECT_EQ(Summary.getAsObject()->getInteger("versions"), 2);
  neverd_lineage_destroy(L);

  const auto Sections = take(neverd_analysis_sections_json(A, 1));
  auto Replayed = input(body());
  ASSERT_EQ(neverd_analysis_sections_import_json(Replayed, Sections.c_str()), 1)
      << take(neverd_last_error(Replayed));
  C = compare(Replayed, B);
  EXPECT_EQ(neverd_comparison_restore_json(C, After.c_str()), 1)
      << take(neverd_comparison_last_error(C));

  ASSERT_EQ(neverd_analysis_section_remove(A, ".extra"), 1);
  C = compare(A, B);
  EXPECT_EQ(neverd_comparison_restore_json(C, Before.c_str()), 1)
      << take(neverd_comparison_last_error(C));
}

TEST_F(BinaryDiffSDK, AnalysisSectionIdentityIgnoresInsertionOrder) {
  auto A = input(body());
  auto B = input(body());
  const uint8_t Data[] = {42, 3, 4};
  const char *Descriptions[] = {
      R"({"schema":"neverd.analysis-section.v1","name":".first",
          "address":"0x2000","permissions":"r"})",
      R"({"schema":"neverd.analysis-section.v1","name":".second",
          "address":"0x3000","permissions":"rw"})"};
  for (unsigned I = 0; I < 2; ++I) {
    ASSERT_EQ(
        neverd_analysis_section_add(A, Descriptions[I], Data, sizeof(Data)), 1);
    ASSERT_EQ(
        neverd_analysis_section_add(B, Descriptions[1 - I], Data, sizeof(Data)),
        1);
  }
  const auto Equal = parsed(neverd_comparison_export_json(compare(A, B)));
  const auto *Left = Equal.getAsObject()->getObject("primary");
  const auto *Right = Equal.getAsObject()->getObject("secondary");
  ASSERT_TRUE(Left && Right);
  EXPECT_EQ(Left->getString("analysis"), Right->getString("analysis"));
  EXPECT_EQ(Left->getString("revision"), Right->getString("revision"));
  const uint8_t Changed[] = {42, 3, 5};
  ASSERT_EQ(neverd_analysis_section_remove(B, ".first"), 1);
  ASSERT_EQ(
      neverd_analysis_section_add(B, Descriptions[0], Changed, sizeof(Changed)),
      1);
  const auto Different = parsed(neverd_comparison_export_json(compare(A, B)));
  Left = Different.getAsObject()->getObject("primary");
  Right = Different.getAsObject()->getObject("secondary");
  ASSERT_TRUE(Left && Right);
  EXPECT_NE(Left->getString("analysis"), Right->getString("analysis"));
  EXPECT_NE(Left->getString("revision"), Right->getString("revision"));
}

TEST_F(BinaryDiffSDK, AnalysisSectionDeclarationsHaveCanonicalIdentity) {
  auto A = input(body());
  auto B = input(body());
  const auto First = body(7), Second = body(9);
  auto Bytes = First;
  Bytes.insert(Bytes.end(), Second.begin(), Second.end());
  const auto Description = [&](bool Reverse) {
    llvm::json::Array Functions{llvm::json::Object{{"name", "added_first"},
                                                   {"offset", "0x0"},
                                                   {"size", First.size()}},
                                llvm::json::Object{{"name", "added_second"},
                                                   {"offset", "0x9"},
                                                   {"size", Second.size()}}};
    if (Reverse)
      std::reverse(Functions.begin(), Functions.end());
    return render(llvm::json::Object{{"schema", "neverd.analysis-section.v1"},
                                     {"name", ".code"},
                                     {"address", "0x3000"},
                                     {"permissions", "rx"},
                                     {"functions", std::move(Functions)}});
  };
  ASSERT_EQ(neverd_analysis_section_add(A, Description(false).c_str(),
                                        Bytes.data(), Bytes.size()),
            1);
  ASSERT_EQ(neverd_analysis_section_add(B, Description(true).c_str(),
                                        Bytes.data(), Bytes.size()),
            1);
  const auto Equal = parsed(neverd_comparison_export_json(compare(A, B)));
  auto *Left = Equal.getAsObject()->getObject("primary");
  auto *Right = Equal.getAsObject()->getObject("secondary");
  ASSERT_TRUE(Left && Right);
  EXPECT_EQ(Left->getString("analysis"), Right->getString("analysis"));
  EXPECT_EQ(Left->getString("revision"), Right->getString("revision"));
  ASSERT_EQ(neverd_analysis_section_remove(B, ".code"), 1);
  const auto DataOnly =
      R"({"schema":"neverd.analysis-section.v1","name":".code",
                           "address":"0x3000","permissions":"rw"})";
  ASSERT_EQ(
      neverd_analysis_section_add(B, DataOnly, Bytes.data(), Bytes.size()), 1);
  // Filtering out added functions must still bind declaration and mapping
  // changes in the rest of the input.
  const auto Different =
      parsed(neverd_comparison_export_json(compare(A, B, R"({"discover":false,
                "primary":{"begin":"0x1000","end":"0x1009"},
                "secondary":{"begin":"0x1000","end":"0x1009"}})")));
  Left = Different.getAsObject()->getObject("primary");
  Right = Different.getAsObject()->getObject("secondary");
  ASSERT_TRUE(Left && Right);
  EXPECT_NE(Left->getString("analysis"), Right->getString("analysis"));
}

TEST_F(BinaryDiffSDK, ByteOverlayIdentityIgnoresEditKindsAndGrouping) {
  const std::array<uint8_t, 24> Bytes{0xb8, 7, 0, 0, 0, 0xc3};
  auto A = input(Bytes);
  auto B = input(Bytes);
  neverd_byte_patch_options_v1 Patch{};
  Patch.struct_size = sizeof(Patch);
  Patch.address = 0x1000;
  Patch.kind = "constant";
  Patch.value = 0x2211;
  ASSERT_FALSE(take(neverd_byte_patch_apply(A, &Patch)).empty())
      << take(neverd_last_error(A));
  Patch.value = 0x11;
  ASSERT_FALSE(take(neverd_byte_patch_apply(B, &Patch)).empty())
      << take(neverd_last_error(B));
  Patch.address = 0x1002;
  Patch.kind = "bytes";
  Patch.bytes = "22";
  ASSERT_FALSE(take(neverd_byte_patch_apply(B, &Patch)).empty())
      << take(neverd_last_error(B));
  const auto ARows = parsed(neverd_byte_patches_json(A));
  const auto BRows = parsed(neverd_byte_patches_json(B));
  ASSERT_TRUE(ARows.getAsArray());
  ASSERT_TRUE(BRows.getAsArray());
  ASSERT_EQ(ARows.getAsArray()->size(), 1u);
  ASSERT_EQ(BRows.getAsArray()->size(), 2u);
  const auto Report = parsed(neverd_comparison_export_json(compare(A, B)));
  const auto *Primary = Report.getAsObject()->getObject("primary");
  const auto *Secondary = Report.getAsObject()->getObject("secondary");
  ASSERT_TRUE(Primary);
  ASSERT_TRUE(Secondary);
  EXPECT_EQ(Primary->getString("analysis"), Secondary->getString("analysis"));
  EXPECT_EQ(Primary->getString("revision"), Secondary->getString("revision"));
}

TEST_F(BinaryDiffSDK, AArch64ProfilesPreservePlacementAndOriginalInstructions) {
  const auto EvidenceSnapshot = [](neverd_comparison_t C) {
    auto Document = parsed(neverd_comparison_export_json(C));
    Document.getAsObject()->erase("timings");
    return render(Document);
  };
  // mov x0, #7; add x0, x0, #3; ret
  const auto Code = words({0xd28000e0, 0x91000c00, 0xd65f03c0});
  auto A = input(Code, 0x1000, "aarch64");
  auto B = input(Code, 0x8000, "aarch64");
  auto C = compare(A, B, R"({"discover":false,"use_names":false})");
  const auto Saved = take(neverd_comparison_export_json(C));
  auto Document = llvm::json::parse(Saved);
  ASSERT_TRUE(bool(Document));
  const auto *Primary = Document->getAsObject()->getObject("primary");
  ASSERT_TRUE(Primary);
  EXPECT_EQ(Primary->getString("architecture"), "aarch64");
  EXPECT_EQ(Primary->getInteger("bits"), 64);
  EXPECT_EQ(Primary->getString("normalization"), "native-aarch64-operands-v1");
  auto Detail = parsed(neverd_comparison_function_json(C, 0, 0x1000));
  const auto *Evidence = Detail.getAsObject()->getObject("evidence");
  ASSERT_TRUE(Evidence) << render(Detail);
  EXPECT_EQ(Evidence->getBoolean("normalized_unchanged"), true);
  const auto *Rows = Detail.getAsObject()->getArray("rows");
  ASSERT_TRUE(Rows);
  ASSERT_EQ(Rows->size(), 3u);
  EXPECT_EQ(
      Rows->front().getAsObject()->getObject("primary")->getString("bytes"),
      "E00080D2");
  const auto Explicit = compare(
      A, B,
      R"({"discover":false,"use_names":false,"profile":"native-aarch64-v1"})");
  EXPECT_EQ(EvidenceSnapshot(Explicit), EvidenceSnapshot(C));
  EXPECT_EQ(neverd_comparison_restore_json(Explicit, Saved.c_str()), 1);
  EXPECT_EQ(neverd_comparison_validate_inputs(Explicit, A, B), 1);
  auto Validation = parsed(neverd_comparison_scalar_validation_json(
      C, A, B, R"({"address":"0x1000"})"));
  EXPECT_EQ(Validation.getAsObject()->getString("status"), "unsupported");

  auto X = input(body()), Y = input(body(9));
  const auto DefaultX86 = compare(X, Y);
  const auto ExplicitX86 =
      compare(X, Y, R"({"discover":false,"profile":"native-x86-v1"})");
  EXPECT_EQ(EvidenceSnapshot(DefaultX86), EvidenceSnapshot(ExplicitX86));
  const auto AutomaticX86 =
      compare(X, Y, R"({"discover":false,"profile":"native-same-isa-v1"})");
  EXPECT_EQ(EvidenceSnapshot(DefaultX86), EvidenceSnapshot(AutomaticX86));
  for (const auto &[Left, Right, Options] :
       {std::tuple{A, B, R"({"discover":false,"profile":"native-x86-v1"})"},
        std::tuple{X, Y, R"({"discover":false,"profile":"native-aarch64-v1"})"},
        std::tuple{A, X, R"({"discover":false})"}}) {
    auto Rejected = neverd_comparison_create();
    Comparisons.push_back(Rejected);
    EXPECT_EQ(
        neverd_comparison_run(Rejected, Left, Right, Options, nullptr, nullptr),
        0);
    EXPECT_FALSE(take(neverd_comparison_last_error(Rejected)).empty());
  }
}

TEST_F(BinaryDiffSDK, AArch64OperandModifiersRemainObservableChanges) {
  // Independently assembled instruction pairs, each with an observable change.
  const struct {
    const char *Name;
    uint32_t A, B;
  } Cases[] = {{"mov x0, #7 / #9", 0xd28000e0, 0xd2800120},
               {"add x0, x1, x2, lsl #1 / #2", 0x8b020420, 0x8b020820},
               {"add x0, x1, w2, uxtw #1 / sxtw #1", 0x8b224420, 0x8b22c420},
               {"ldr x0, [x1, #8] / [x1, #8]!", 0xf9400420, 0xf8408c20},
               {"ldr x0, [x1, #8]! / [x1], #8", 0xf8408c20, 0xf8408420},
               {"csel x0, x1, x2, eq / ne", 0x9a820020, 0x9a821020},
               {"fadd v0.4s, v1.4s, v2.4s / v0.2s, v1.2s, v2.2s", 0x4e22d420,
                0x0e22d420},
               {"fmov d0, #1.0 / #2.0", 0x1e6e1000, 0x1e601000},
               {"mrs x0, nzcv / fpcr", 0xd53b4200, 0xd53b4400},
               {"CPYFP register tuples", 0x19010440, 0x190404a3},
               {"lsl x0, x1, x2 / x3", 0x9ac22020, 0x9ac32020},
               {"lsr x0, x1, x2 / x3", 0x9ac22420, 0x9ac32420},
               {"asr x0, x1, x2 / x3", 0x9ac22820, 0x9ac32820},
               {"ror x0, x1, x2 / x3", 0x9ac22c20, 0x9ac32c20},
               {"SVE merging / zeroing", 0x05904020, 0x05900020}};
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    auto A = input(words({0xd28000e3, Case.A, 0x91000c00, 0xd65f03c0}), 0x1000,
                   "aarch64");
    auto B = input(words({0xd28000e3, Case.B, 0x91000c00, 0xd65f03c0}), 0x8000,
                   "aarch64");
    auto C = compare(
        A, B,
        R"({"discover":false,"overrides":[{"kind":"match","primary":"0x1000","secondary":"0x8000"}]})");
    auto Detail = parsed(neverd_comparison_function_json(C, 0, 0x1000));
    const auto *Evidence = Detail.getAsObject()->getObject("evidence");
    ASSERT_TRUE(Evidence) << render(Detail);
    EXPECT_EQ(Evidence->getBoolean("normalized_unchanged"), false);
    auto Saved = parsed(neverd_comparison_export_json(C));
    ASSERT_TRUE(Saved.getAsObject()->getArray("matches"));
    EXPECT_EQ(Saved.getAsObject()
                  ->getArray("matches")
                  ->front()
                  .getAsObject()
                  ->getBoolean("analysis_complete"),
              true);
  }
}

TEST_F(BinaryDiffSDK, UnpairedMOVPRFXCannotReportCompleteSemanticComparison) {
  using namespace sve_compare_fixture;
  auto A = input(words({kSetup, kMerging, kIncompatibleNext, kReturn}),
                 kPrimaryBase, kProcessor);
  auto B = input(words({kSetup, kZeroing, kIncompatibleNext, kReturn}),
                 kSecondaryBase, kProcessor);
  auto Comparison = compare(A, B, kOptions);
  auto Detail =
      parsed(neverd_comparison_function_json(Comparison, 0, kPrimaryBase));
  const auto *Evidence = Detail.getAsObject()->getObject(kEvidence);
  ASSERT_TRUE(Evidence) << render(Detail);
  EXPECT_EQ(Evidence->getBoolean(kNormalizedUnchanged), false);
  EXPECT_EQ(Evidence->getBoolean(kAnalysisComplete), false);
  auto Saved = parsed(neverd_comparison_export_json(Comparison));
  const auto *Matches = Saved.getAsObject()->getArray(kMatches);
  ASSERT_TRUE(Matches);
  ASSERT_FALSE(Matches->empty());
  EXPECT_EQ(Matches->front().getAsObject()->getBoolean(kAnalysisComplete),
            false);
}

TEST_F(BinaryDiffSDK, AArch64BranchBitIndexIsNotASecondTargetOperand) {
  // At base 0, TBZ's bit index and target both equal 12. At base 8 they both
  // equal 20. The CFG and relative branch displacement agree, but the tested
  // bit changes. Numeric equality must not erase that change.
  auto A =
      input(words({0x36600060, 0xd28000e0, 0xd65f03c0, 0xd2800120, 0xd65f03c0}),
            0, "aarch64");
  auto B =
      input(words({0x36a00060, 0xd28000e0, 0xd65f03c0, 0xd2800120, 0xd65f03c0}),
            8, "aarch64");
  auto C = compare(
      A, B,
      R"({"discover":false,"overrides":[{"kind":"match","primary":"0x0","secondary":"0x8"}]})");
  auto Detail = parsed(neverd_comparison_function_json(C, 0, 0));
  const auto *Evidence = Detail.getAsObject()->getObject("evidence");
  ASSERT_TRUE(Evidence) << render(Detail);
  EXPECT_EQ(Evidence->getBoolean("normalized_unchanged"), false);
}

TEST_F(BinaryDiffSDK,
       AArch64UnsizedConditionalFallthroughKeepsChangesAndCalls) {
  // cmp w0, #17; b.eq taken; mov w0, #7/#9; bl callee; ret;
  // taken: ret; callee: ret. Both exits belong to the unsized caller.
  auto A = input(words({0x7100441f, 0x54000080, 0x528000e0, 0x94000003,
                        0xd65f03c0, 0xd65f03c0, 0xd65f03c0}),
                 0x1000, "aarch64");
  auto B = input(words({0x7100441f, 0x54000080, 0x52800120, 0x94000003,
                        0xd65f03c0, 0xd65f03c0, 0xd65f03c0}),
                 0x8000, "aarch64");
  ASSERT_EQ(neverd_func_create(A, 0x1018), 0);
  ASSERT_EQ(neverd_func_create(B, 0x8018), 0);
  auto C = compare(A, B);
  auto Detail = parsed(neverd_comparison_function_json(C, 0, 0x1000));
  const auto *Evidence = Detail.getAsObject()->getObject("evidence");
  ASSERT_TRUE(Evidence) << render(Detail);
  EXPECT_EQ(Evidence->getBoolean("analysis_complete"), true);
  EXPECT_EQ(Evidence->getBoolean("normalized_unchanged"), false);
  const auto *Rows = Detail.getAsObject()->getArray("rows");
  ASSERT_TRUE(Rows);
  EXPECT_EQ(Rows->size(), 6u);
  EXPECT_TRUE(std::any_of(Rows->begin(), Rows->end(), [](const auto &Row) {
    const auto *O = Row.getAsObject();
    const auto *Primary = O->getObject("primary");
    return O->getString("kind") == "constant" && Primary &&
           Primary->getString("address") == "0x1008";
  }));
  auto References = parsed(neverd_code_refs_json(A, 0x1000, 1));
  const auto *Refs = References.getAsObject()->getArray("refs");
  ASSERT_TRUE(Refs);
  EXPECT_TRUE(std::any_of(Refs->begin(), Refs->end(), [](const auto &Ref) {
    const auto &Fields = *Ref.getAsArray();
    return Fields[0].getAsString() == "0x100C" &&
           Fields[1].getAsString() == "0x1018" &&
           Fields[2].getAsString() == "call";
  })) << render(References);
}

TEST_F(BinaryDiffSDK,
       AArch64IllegalInstructionsRetainBytesWithoutCompleteFacts) {
  auto A =
      input(words({0xf8408c00, 0x91000c00, 0xd65f03c0}), 0x1000, "aarch64");
  auto B =
      input(words({0xf8408c00, 0x91000c00, 0xd65f03c0}), 0x8000, "aarch64");
  auto C = compare(A, B);
  auto Document = parsed(neverd_comparison_export_json(C));
  const auto *Matches = Document.getAsObject()->getArray("matches");
  ASSERT_TRUE(Matches);
  ASSERT_EQ(Matches->size(), 1u);
  EXPECT_EQ(Matches->front().getAsObject()->getBoolean("analysis_complete"),
            false);
  auto Detail = parsed(neverd_comparison_function_json(C, 0, 0x1000));
  const auto *Evidence = Detail.getAsObject()->getObject("evidence");
  ASSERT_TRUE(Evidence);
  EXPECT_EQ(Evidence->getBoolean("normalized_unchanged"), false);
  const auto *Rows = Detail.getAsObject()->getArray("rows");
  ASSERT_TRUE(Rows);
  ASSERT_FALSE(Rows->empty());
  const auto *Original = Rows->front().getAsObject()->getObject("primary");
  ASSERT_TRUE(Original);
  EXPECT_EQ(Original->getString("bytes"), "008C40F8");
  EXPECT_EQ(render(*Original->get("target")), "null");
  EXPECT_EQ(render(*Original->get("callee")), "null");
  auto Listing =
      parsed(neverd_disasm_json_ex(A, 0x1000, 2, NEVERD_DISASM_FLOW));
  ASSERT_TRUE(Listing.getAsArray());
  ASSERT_EQ(Listing.getAsArray()->size(), 2u);
  const auto *First = Listing.getAsArray()->front().getAsObject();
  EXPECT_EQ(First->getString("flow"), "unlifted");
  EXPECT_FALSE(First->get("target"));
  EXPECT_FALSE(First->get("refs"));
  EXPECT_FALSE(Listing.getAsArray()->back().getAsObject()->get("flow"));
}

TEST_F(BinaryDiffSDK,
       AArch64PCRelativeEvidenceKeepsFullAddressesAndPageFragments) {
  // ADR x0, .+16; ADRP x1, current page; LDR x0, .+16; LDR x0, .-4; RET.
  const auto Code =
      words({0x10000080, 0x90000001, 0x58000080, 0x58ffffe0, 0xd65f03c0});
  auto A = input(Code, 0x100000000, "aarch64");
  auto B = input(Code, 0x200000000, "aarch64");
  auto C = compare(A, B);
  auto Detail = parsed(neverd_comparison_function_json(C, 0, 0x100000000));
  const auto *Evidence = Detail.getAsObject()->getObject("evidence");
  ASSERT_TRUE(Evidence) << render(Detail);
  EXPECT_EQ(Evidence->getBoolean("normalized_unchanged"), true);

  // A partial-page move changes ADRP's fragment relative to the image's data.
  auto Shifted = input(Code, 0x200000004, "aarch64");
  auto NearMiss = compare(
      A, Shifted,
      R"({"discover":false,"overrides":[{"kind":"match","primary":"0x100000000","secondary":"0x200000004"}]})");
  auto Changed =
      parsed(neverd_comparison_function_json(NearMiss, 0, 0x100000000));
  const auto *ChangedEvidence = Changed.getAsObject()->getObject("evidence");
  ASSERT_TRUE(ChangedEvidence) << render(Changed);
  EXPECT_EQ(ChangedEvidence->getBoolean("normalized_unchanged"), false);
}

TEST_F(BinaryDiffSDK, OwnedSourcePagesRetainEmitterTextAndOriginalAnchors) {
  auto A = input(body(7)), B = input(body(9), 0x8000);
  const auto Ordinary = parsed(neverd_ir_view_json(A, 0x1000, "c", 0, 200));
  ASSERT_TRUE(Ordinary.getAsObject()->getString("text"));
  const auto C = neverd_comparison_create();
  Comparisons.push_back(C);
  ASSERT_EQ(neverd_comparison_run_owned(C, &Sessions[0], &Sessions[1],
                                        R"({"discover":false})", nullptr,
                                        nullptr),
            1);
  const auto Saved = take(neverd_comparison_export_json(C));
  auto Listing = parsed(
      neverd_comparison_function_page_json(C, R"({"address":"0x1000"})"));
  ASSERT_TRUE(Listing.getAsObject()->getArray("rows"));
  // Source requests use the retained inputs, even after their paths disappear.
  ASSERT_TRUE(std::filesystem::remove(Directory / "0.raw"));
  ASSERT_TRUE(std::filesystem::remove(Directory / "1.raw"));
  auto Page = parsed(neverd_comparison_source_page_json(
      C, R"({"address":"0x1000","representation":"c"})"));
  const auto *Root = Page.getAsObject();
  ASSERT_TRUE(Root);
  ASSERT_EQ(Root->getString("availability"), "available") << render(Page);
  const auto *Doc = Root->getObject("document");
  ASSERT_TRUE(Doc);
  EXPECT_EQ(Doc->getString("text"), Ordinary.getAsObject()->getString("text"));
  EXPECT_EQ(Root->getString("input_source_binding"), "owned_sessions");
  EXPECT_EQ(Root->getString("semantic_validation"), "not_performed");
  EXPECT_EQ(Root->getString("source_alignment"), "not_performed");
  EXPECT_FALSE(Root->getBoolean("provenance_complete").value_or(true));
  EXPECT_FALSE(Root->getBoolean("hard_deadline").value_or(true));
  unsigned Anchored = 0, Unmapped = 0;
  for (const auto &Row : *Doc->getArray("rows")) {
    const auto *R = Row.getAsObject();
    const auto *Anchors = R->getArray("comparison_anchors");
    ASSERT_TRUE(Anchors);
    if (R->getString("mapping_status") == "unmapped") {
      EXPECT_TRUE(Anchors->empty());
      ++Unmapped;
    }
    for (const auto &V : *Anchors) {
      const auto *Anchor = V.getAsObject();
      const auto NativeRow = Anchor->getInteger("listing_row");
      ASSERT_TRUE(NativeRow);
      const auto &Instruction =
          (*Listing.getAsObject()->getArray("rows"))[*NativeRow];
      EXPECT_EQ(Anchor->getString("address"),
                Instruction.getAsObject()->getObject("primary")->getString(
                    "address"));
      EXPECT_EQ(Anchor->getString("native_change"),
                Instruction.getAsObject()->getString("kind"));
      ++Anchored;
    }
  }
  EXPECT_GT(Anchored, 0u);
  EXPECT_GT(Unmapped, 0u);
  const auto Total = Doc->getInteger("total_lines");
  ASSERT_TRUE(Total);
  std::string Text;
  for (int64_t Offset = 0; Offset < *Total;) {
    const auto Query = render(llvm::json::Object{
        {"address", "0x1000"}, {"offset", Offset}, {"limit", 2}});
    const auto Part =
        parsed(neverd_comparison_source_page_json(C, Query.c_str()));
    const auto *D = Part.getAsObject()->getObject("document");
    ASSERT_TRUE(D);
    ASSERT_EQ(D->getInteger("offset"), Offset);
    Text += D->getString("text").value_or("");
    const auto Count = D->getArray("rows")->size();
    ASSERT_GT(Count, 0u);
    ASSERT_LE(Count, 2u);
    Offset += Count;
  }
  EXPECT_EQ(Text, Doc->getString("text"));
  const auto Other = parsed(neverd_comparison_source_page_json(
      C,
      R"({"side":"secondary","address":"0x8000","representation":"llvmc"})"));
  ASSERT_EQ(Other.getAsObject()->getString("availability"), "available")
      << render(Other);
  EXPECT_EQ(Other.getAsObject()->getObject("function")->getString("address"),
            "0x8000");
  EXPECT_EQ(take(neverd_comparison_export_json(C)), Saved);
}

TEST_F(BinaryDiffSDK, SourceAdmissionAndUnavailableStatesStayExplicit) {
  auto A = input(body()), B = input(body());
  auto Borrowed = compare(A, B);
  EXPECT_EQ(
      neverd_comparison_source_page_json(Borrowed, R"({"address":"0x1000"})"),
      nullptr);
  auto C = neverd_comparison_create();
  Comparisons.push_back(C);
  ASSERT_EQ(neverd_comparison_run_owned(C, &Sessions[0], &Sessions[1],
                                        R"({"discover":false})", nullptr,
                                        nullptr),
            1);
  for (const auto *Query :
       {R"({"address":"0xdead"})", R"({"address":"0x1000","limit":0})",
        R"({"address":"0x1000","limit":2049})",
        R"({"address":"0x1000","offset":-1})",
        R"({"address":"0x1000","representation":"llvm"})",
        R"({"address":"0x1000","bogus":true})"})
    EXPECT_EQ(neverd_comparison_source_page_json(C, Query), nullptr) << Query;
  const auto Limited = parsed(neverd_comparison_source_page_json(
      C, R"({"address":"0x1000","max_native_instructions":1})"));
  EXPECT_EQ(Limited.getAsObject()->getString("state"), "resource_limited");
  EXPECT_TRUE(Limited.getAsObject()->getObject("document")->empty());
  const auto Retry = parsed(neverd_comparison_source_page_json(
      C, R"({"address":"0x1000","max_native_instructions":0})"));
  EXPECT_EQ(Retry.getAsObject()->getString("availability"), "available")
      << render(Retry);
  const auto Missing = parsed(neverd_comparison_source_page_json(
      C, R"({"address":"0x1000","representation":"rust"})"));
  EXPECT_EQ(Missing.getAsObject()->getString("availability"), "unavailable")
      << render(Missing);
  EXPECT_FALSE(Missing.getAsObject()->getString("reason")->empty());
  EXPECT_TRUE(Missing.getAsObject()->getObject("document")->empty());
}

TEST_F(BinaryDiffSDK, SourceCancellationDiscardsTextAndKeepsNativeComparison) {
  std::vector<uint8_t> Code{0x89, 0xf8}; // mov eax, edi
  for (unsigned I = 0; I < 1500; ++I)
    Code.insert(Code.end(), {0x83, 0xc0, 1});
  Code.push_back(0xc3);
  input(Code);
  input(Code, 0x8000);
  auto C = neverd_comparison_create();
  Comparisons.push_back(C);
  ASSERT_EQ(neverd_comparison_run_owned(C, &Sessions[0], &Sessions[1],
                                        R"({"discover":false})", nullptr,
                                        nullptr),
            1);
  const auto Before = take(neverd_comparison_export_json(C));
  auto Work = std::async(std::launch::async, [&] {
    return take(
        neverd_comparison_source_page_json(C, R"({"address":"0x1000"})"));
  });
  bool Requested = false;
  while (Work.wait_for(std::chrono::milliseconds(0)) !=
         std::future_status::ready) {
    const auto Status = parsed(neverd_comparison_status_json(C));
    if (Status.getAsObject()->getBoolean("running").value_or(false)) {
      neverd_comparison_cancel(C);
      Requested = true;
      break;
    }
    std::this_thread::yield();
  }
  ASSERT_TRUE(Requested);
  auto Stopped = llvm::json::parse(Work.get());
  ASSERT_TRUE(static_cast<bool>(Stopped));
  EXPECT_EQ(Stopped->getAsObject()->getString("state"), "cancelled");
  EXPECT_TRUE(Stopped->getAsObject()->getObject("document")->empty());
  EXPECT_EQ(take(neverd_comparison_export_json(C)), Before);
  const auto TimedOut = parsed(neverd_comparison_source_page_json(
      C, R"({"address":"0x1000","refresh":true,"timeout_ms":1})"));
  EXPECT_EQ(TimedOut.getAsObject()->getString("state"), "timed_out")
      << render(TimedOut);
  EXPECT_TRUE(TimedOut.getAsObject()->getObject("document")->empty());
  EXPECT_EQ(take(neverd_comparison_export_json(C)), Before);
  const auto Retry = parsed(neverd_comparison_source_page_json(
      C, R"({"address":"0x1000","limit":2})"));
  EXPECT_EQ(Retry.getAsObject()->getString("availability"), "available")
      << render(Retry);
  EXPECT_EQ(take(neverd_comparison_export_json(C)), Before);
}

TEST_F(BinaryDiffSDK, DeepSourceAnalysisCompletesOnAnAsyncCallerStack) {
  std::vector<uint8_t> Code{0x89, 0xf8};
  for (unsigned I = 0; I < 1500; ++I)
    Code.insert(Code.end(), {0x83, 0xc0, 1});
  Code.push_back(0xc3);
  input(Code);
  input(Code, 0x8000);
  auto C = neverd_comparison_create();
  Comparisons.push_back(C);
  ASSERT_EQ(neverd_comparison_run_owned(C, &Sessions[0], &Sessions[1],
                                        R"({"discover":false})", nullptr,
                                        nullptr),
            1);
  // Unlike cancellation coverage, this completes every analysis phase even
  // when the caller wins the scheduling race. macOS worker stacks are smaller
  // than the main thread's stack.
  auto Work = std::async(std::launch::async, [&] {
    return take(neverd_comparison_source_page_json(
        C, R"({"address":"0x1000","limit":2})"));
  });
  auto Page = llvm::json::parse(Work.get());
  ASSERT_TRUE(bool(Page));
  ASSERT_TRUE(Page->getAsObject());
  EXPECT_EQ(Page->getAsObject()->getString("availability"), "available")
      << render(*Page);
}

TEST_F(BinaryDiffSDK, SourceDifferencePagesReassembleExactOwnedRenderings) {
  auto A = input(body(7)), B = input(body(9), 0x8000);
  const auto OrdinaryA = parsed(neverd_ir_view_json(A, 0x1000, "c", 0, 200));
  const auto OrdinaryB = parsed(neverd_ir_view_json(B, 0x8000, "c", 0, 200));
  auto C = neverd_comparison_create();
  Comparisons.push_back(C);
  ASSERT_EQ(neverd_comparison_run_owned(C, &Sessions[0], &Sessions[1],
                                        R"({"discover":false})", nullptr,
                                        nullptr),
            1);
  const auto Saved = take(neverd_comparison_export_json(C));
  auto Native = parsed(
      neverd_comparison_function_page_json(C, R"({"address":"0x1000"})"));
  ASSERT_TRUE(std::filesystem::remove(Directory / "0.raw"));
  ASSERT_TRUE(std::filesystem::remove(Directory / "1.raw"));
  std::string Text[2];
  uint64_t Lines[2] = {};
  bool Changed = false, Anchored = false;
  for (uint64_t Offset = 0;;) {
    const auto Query =
        render(llvm::json::Object{{"side", Offset ? "secondary" : "primary"},
                                  {"address", Offset ? "0x8000" : "0x1000"},
                                  {"include_prelude", true},
                                  {"offset", Offset},
                                  {"limit", 2}});
    auto Page =
        parsed(neverd_comparison_source_difference_json(C, Query.c_str()));
    const auto *Root = Page.getAsObject();
    ASSERT_TRUE(Root);
    ASSERT_EQ(Root->getString("availability"), "available") << render(Page);
    EXPECT_EQ(Root->getBoolean("cached"), Offset != 0);
    EXPECT_EQ(Root->getString("semantic_validation"), "not_performed");
    EXPECT_EQ(Root->getString("source_alignment"), "exact_text_lines");
    EXPECT_EQ(Root->getBoolean("computation_complete"), true);
    const auto *Rows = Root->getArray("rows");
    ASSERT_TRUE(Rows);
    ASSERT_LE(Rows->size(), 2u);
    for (const auto &Value : *Rows) {
      const auto &Row = *Value.getAsObject();
      Changed |= Row.getString("kind") == "changed";
      for (unsigned Side = 0; Side < 2; ++Side) {
        const auto Key = Side ? "secondary" : "primary";
        if (const auto *Line = Row.getObject(Key)) {
          EXPECT_EQ(Line->getInteger("line"), Lines[Side]++);
          Text[Side] += Line->getString("text").value_or("");
          for (const auto &V : *Line->getArray("comparison_anchors")) {
            const auto &Anchor = *V.getAsObject();
            const auto ListingRow = Anchor.getInteger("listing_row");
            ASSERT_TRUE(ListingRow);
            const auto &NativeRow =
                (*Native.getAsObject()->getArray("rows"))[*ListingRow];
            EXPECT_EQ(
                Anchor.getString("address"),
                NativeRow.getAsObject()->getObject(Key)->getString("address"));
            Anchored = true;
          }
        }
      }
      if (Row.getString("kind") == "unchanged") {
        ASSERT_TRUE(Row.getObject("primary"));
        ASSERT_TRUE(Row.getObject("secondary"));
        EXPECT_EQ(Row.getObject("primary")->getString("text"),
                  Row.getObject("secondary")->getString("text"));
      }
    }
    const auto Next = Root->getInteger("next_offset");
    if (!Next)
      break;
    ASSERT_GT(*Next, Offset);
    Offset = *Next;
  }
  EXPECT_EQ(Text[0], OrdinaryA.getAsObject()->getString("text"));
  EXPECT_EQ(Text[1], OrdinaryB.getAsObject()->getString("text"));
  EXPECT_TRUE(Changed);
  EXPECT_TRUE(Anchored);
  EXPECT_EQ(take(neverd_comparison_export_json(C)), Saved);
  const auto Body = parsed(
      neverd_comparison_source_difference_json(C, R"({"address":"0x1000"})"));
  EXPECT_EQ(Body.getAsObject()->getString("scope"), "function_body");
  EXPECT_GT(Body.getAsObject()->getObject("primary")->getInteger("first_line"),
            0);
  EXPECT_EQ(
      neverd_comparison_override_json(
          C, R"({"kind":"unmatch","primary":"0x1000","secondary":"0x8000"})"),
      1);
  EXPECT_EQ(
      neverd_comparison_source_difference_json(C, R"({"address":"0x1000"})"),
      nullptr);
  EXPECT_EQ(neverd_comparison_undo(C), 1);
  const auto Restored = parsed(
      neverd_comparison_source_difference_json(C, R"({"address":"0x1000"})"));
  EXPECT_EQ(Restored.getAsObject()->getBoolean("cached"), false);
  EXPECT_EQ(Restored.getAsObject()->getString("availability"), "available");
}

TEST_F(BinaryDiffSDK, SourceDifferenceLimitsDoNotClassifyPartialRenderings) {
  auto A = input(body()), B = input(body(), 0x8000);
  auto Borrowed = compare(A, B);
  EXPECT_EQ(neverd_comparison_source_difference_json(Borrowed,
                                                     R"({"address":"0x1000"})"),
            nullptr);
  auto C = neverd_comparison_create();
  Comparisons.push_back(C);
  ASSERT_EQ(neverd_comparison_run_owned(C, &Sessions[0], &Sessions[1],
                                        R"({"discover":false})", nullptr,
                                        nullptr),
            1);
  const auto Saved = take(neverd_comparison_export_json(C));
  for (const auto *Query :
       {R"({"address":"0x1000","max_source_bytes":1})",
        R"({"address":"0x1000","max_work":1})",
        R"({"address":"0x1000","max_native_instructions":1})"}) {
    auto Page = parsed(neverd_comparison_source_difference_json(C, Query));
    EXPECT_EQ(Page.getAsObject()->getString("state"), "resource_limited");
    EXPECT_EQ(Page.getAsObject()->getBoolean("computation_complete"), false);
    EXPECT_TRUE(Page.getAsObject()->getArray("rows")->empty());
    EXPECT_EQ(
        Page.getAsObject()->getObject("primary")->getInteger("total_lines"), 0);
    auto Cached = parsed(neverd_comparison_source_difference_json(C, Query));
    EXPECT_EQ(Cached.getAsObject()->getBoolean("cached"), true);
  }
  const auto Unlimited = parsed(neverd_comparison_source_difference_json(
      C,
      R"({"address":"0x1000","max_source_bytes":0,"max_work":0,"max_native_instructions":0})"));
  ASSERT_EQ(Unlimited.getAsObject()->getString("availability"), "available")
      << render(Unlimited);
  const auto Bytes = *Unlimited.getAsObject()->getInteger("source_bytes");
  const auto Work = *Unlimited.getAsObject()->getInteger("work");
  for (const auto &[Key, Bound] :
       {std::pair{"max_source_bytes", Bytes}, std::pair{"max_work", Work}}) {
    for (const auto Exact : {false, true}) {
      const auto Query = render(llvm::json::Object{
          {"address", "0x1000"}, {Key, Bound - (Exact ? 0 : 1)}});
      auto Page =
          parsed(neverd_comparison_source_difference_json(C, Query.c_str()));
      EXPECT_EQ(Page.getAsObject()->getBoolean("computation_complete"), Exact);
    }
  }
  for (const auto *Query : {R"({"address":"0x1000","limit":0})",
                            R"({"address":"0x1000","limit":2049})",
                            R"({"address":"0x1000","max_work":-1})",
                            R"({"address":"0x1000","max_source_bytes":"0"})",
                            R"({"address":"0x1000","include_prelude":1})",
                            R"({"address":"0x1000","representation":"llvm"})",
                            R"({"address":"0x1000","surprise":true})"})
    EXPECT_EQ(neverd_comparison_source_difference_json(C, Query), nullptr)
        << Query;
  auto Missing = parsed(neverd_comparison_source_difference_json(
      C, R"({"address":"0x1000","representation":"rust"})"));
  EXPECT_EQ(Missing.getAsObject()->getString("availability"), "unavailable");
  EXPECT_TRUE(Missing.getAsObject()->getArray("rows")->empty());
  EXPECT_EQ(take(neverd_comparison_export_json(C)), Saved);
}

TEST_F(BinaryDiffSDK, SourceDifferenceCollectsBeyondAnEmitterPage) {
  // Distinct observable stores survive ordinary source simplification. The
  // changed store lies beyond the emitter's first 2048-line page.
  const auto Stores = [](bool Changed) {
    std::vector<uint8_t> Code;
    for (unsigned I = 0; I < 2200; ++I) {
      Code.insert(Code.end(),
                  {0xc7, 0x87}); // mov dword ptr [rdi+disp32], imm32
      for (unsigned Shift = 0; Shift < 32; Shift += 8)
        Code.push_back(uint8_t((I * 4) >> Shift));
      const unsigned Value = I + (Changed && I == 2150 ? 1 : 0);
      for (unsigned Shift = 0; Shift < 32; Shift += 8)
        Code.push_back(uint8_t(Value >> Shift));
    }
    Code.push_back(0xc3);
    return Code;
  };
  input(Stores(false));
  input(Stores(true));
  auto C = neverd_comparison_create();
  Comparisons.push_back(C);
  ASSERT_EQ(neverd_comparison_run_owned(C, &Sessions[0], &Sessions[1],
                                        R"({"discover":false})", nullptr,
                                        nullptr),
            1);
  std::string Aligned[2];
  bool Changed = false;
  for (uint64_t Offset = 0;;) {
    const auto Query = render(llvm::json::Object{{"address", "0x1000"},
                                                 {"include_prelude", true},
                                                 {"offset", Offset},
                                                 {"limit", 999}});
    auto Page =
        parsed(neverd_comparison_source_difference_json(C, Query.c_str()));
    const auto *Root = Page.getAsObject();
    ASSERT_EQ(Root->getString("availability"), "available") << render(Page);
    ASSERT_GT(Root->getInteger("total_rows"), 2048);
    EXPECT_EQ(Root->getBoolean("cached"), Offset != 0);
    for (const auto &Value : *Root->getArray("rows")) {
      const auto &Row = *Value.getAsObject();
      Changed |= Row.getString("kind") == "changed";
      for (unsigned Side = 0; Side < 2; ++Side)
        if (const auto *Line = Row.getObject(Side ? "secondary" : "primary"))
          Aligned[Side] += Line->getString("text").value_or("");
    }
    const auto Next = Root->getInteger("next_offset");
    if (!Next)
      break;
    ASSERT_GT(*Next, Offset);
    Offset = *Next;
  }
  EXPECT_TRUE(Changed);
  for (unsigned Side = 0; Side < 2; ++Side) {
    std::string Ordinary;
    for (uint64_t Offset = 0;;) {
      const auto Query =
          render(llvm::json::Object{{"address", "0x1000"},
                                    {"side", Side ? "secondary" : "primary"},
                                    {"offset", Offset},
                                    {"limit", 311}});
      auto Page = parsed(neverd_comparison_source_page_json(C, Query.c_str()));
      ASSERT_EQ(Page.getAsObject()->getString("availability"), "available");
      const auto *Document = Page.getAsObject()->getObject("document");
      Ordinary += Document->getString("text").value_or("");
      const auto Next = Document->getInteger("next_offset");
      if (!Next)
        break;
      ASSERT_GT(*Next, Offset);
      Offset = *Next;
    }
    EXPECT_EQ(Aligned[Side], Ordinary);
  }
}

TEST_F(BinaryDiffSDK, SourceDifferenceCancellationAndDeadlineRemainRetryable) {
  std::vector<uint8_t> Code{0x89, 0xf8};
  for (unsigned I = 0; I < 1500; ++I)
    Code.insert(Code.end(), {0x83, 0xc0, 1});
  Code.push_back(0xc3);
  input(Code);
  input(Code, 0x8000);
  auto C = neverd_comparison_create();
  Comparisons.push_back(C);
  ASSERT_EQ(neverd_comparison_run_owned(C, &Sessions[0], &Sessions[1],
                                        R"({"discover":false})", nullptr,
                                        nullptr),
            1);
  const auto Saved = take(neverd_comparison_export_json(C));
  auto Running = std::async(std::launch::async, [&] {
    return take(
        neverd_comparison_source_difference_json(C, R"({"address":"0x1000"})"));
  });
  bool Cancelled = false;
  while (Running.wait_for(std::chrono::milliseconds(0)) !=
         std::future_status::ready) {
    auto Status = parsed(neverd_comparison_status_json(C));
    if (Status.getAsObject()->getBoolean("running").value_or(false)) {
      neverd_comparison_cancel(C);
      Cancelled = true;
      break;
    }
    std::this_thread::yield();
  }
  ASSERT_TRUE(Cancelled);
  auto Stopped = llvm::json::parse(Running.get());
  ASSERT_TRUE(static_cast<bool>(Stopped));
  EXPECT_EQ(Stopped->getAsObject()->getString("state"), "cancelled");
  EXPECT_TRUE(Stopped->getAsObject()->getArray("rows")->empty());
  auto Cached = parsed(
      neverd_comparison_source_difference_json(C, R"({"address":"0x1000"})"));
  EXPECT_EQ(Cached.getAsObject()->getString("state"), "cancelled");
  EXPECT_EQ(Cached.getAsObject()->getBoolean("cached"), true);
  auto Timeout = parsed(neverd_comparison_source_difference_json(
      C, R"({"address":"0x1000","refresh":true,"timeout_ms":1})"));
  EXPECT_EQ(Timeout.getAsObject()->getString("state"), "timed_out")
      << render(Timeout);
  EXPECT_TRUE(Timeout.getAsObject()->getArray("rows")->empty());
  auto Retry = parsed(neverd_comparison_source_difference_json(
      C, R"({"address":"0x1000","refresh":true})"));
  EXPECT_EQ(Retry.getAsObject()->getString("availability"), "available")
      << render(Retry);
  EXPECT_EQ(take(neverd_comparison_export_json(C)), Saved);
}

TEST_F(BinaryDiffSDK, AmbiguityInventoryPagesEveryRetainedCandidate) {
  constexpr unsigned Count = 40, Stride = 16;
  const auto Copies = [&](uint64_t Base, const std::string &Prefix,
                          bool Changed) {
    std::vector<uint8_t> Bytes(Count * Stride, 0xcc);
    for (unsigned I = 0; I < Count; ++I) {
      const auto Code = body();
      std::copy(Code.begin(), Code.end(), Bytes.begin() + I * Stride);
    }
    if (Changed)
      Bytes.back() = 0xdd; // Different file, identical returning bodies.
    const auto S = input(Bytes, Base);
    EXPECT_GE(neverd_session_discover_functions(S), Count);
    EXPECT_GE(neverd_func_count(S), Count);
    for (unsigned I = 0; I < Count; ++I) {
      EXPECT_GE(neverd_func_find_by_addr(S, Base + I * Stride), 0);
      EXPECT_EQ(neverd_rename_addr(S, Base + I * Stride,
                                   (Prefix + std::to_string(I)).c_str()),
                0);
    }
    return S;
  };
  const auto A = Copies(0x1000, "before_copy_", false);
  const auto B = Copies(0x8000, "after_copy_", true);
  // Zero relaxes the required positive lead; it must not turn exact ties
  // into arbitrary matches and make their full candidate inventory disappear.
  const auto C = compare(
      A, B, R"({"discover":false,"use_names":false,"ambiguity_margin":0})");
  const auto Saved = take(neverd_comparison_export_json(C));
  const auto Query = [&](llvm::json::Object Options) {
    const auto Text = render(std::move(Options));
    return parsed(neverd_comparison_functions_json(C, Text.c_str()));
  };
  for (const bool Secondary : {false, true}) {
    SCOPED_TRACE(Secondary);
    const auto Side = Secondary ? "secondary" : "primary";
    const auto Anchor = Secondary ? "0x1010" : "0x8010";
    auto First = Query(llvm::json::Object{
        {"side", Side}, {"ambiguous_with", Anchor}, {"limit", 7}});
    ASSERT_TRUE(First.getAsObject());
    const auto *O = First.getAsObject();
    const auto Total = O->getInteger("total").value_or(0);
    ASSERT_GE(Total, Count - 1) << render(First);
    ASSERT_TRUE(O->getObject("ambiguity"));
    EXPECT_TRUE(
        O->getObject("ambiguity")->getBoolean("active").value_or(false));
    EXPECT_EQ(O->getObject("ambiguity")->getInteger("candidate_total"), Total);
    EXPECT_EQ(O->getObject("ambiguity")->getString("source_side"),
              Secondary ? "primary" : "secondary");
    std::vector<std::string> Addresses;
    for (int64_t Offset = 0; Offset < Total; Offset += 7) {
      auto Page = Query(llvm::json::Object{{"side", Side},
                                           {"ambiguous_with", Anchor},
                                           {"offset", Offset},
                                           {"limit", 7}});
      ASSERT_TRUE(Page.getAsObject()->getArray("items"));
      const auto &Items = *Page.getAsObject()->getArray("items");
      ASSERT_EQ(Items.size(), std::min<int64_t>(7, Total - Offset));
      for (const auto &Row : Items) {
        const auto Address = Row.getAsObject()->getString("address");
        ASSERT_TRUE(Address);
        if (!Addresses.empty())
          EXPECT_LT(Addresses.back(), Address->str());
        Addresses.push_back(Address->str());
      }
      if (Offset + 7 < Total)
        EXPECT_EQ(Page.getAsObject()->getInteger("next_offset"), Offset + 7);
      else
        EXPECT_FALSE(Page.getAsObject()->getInteger("next_offset"));
    }
    ASSERT_EQ(Addresses.size(), Total);
    auto End = Query(llvm::json::Object{
        {"side", Side}, {"ambiguous_with", Anchor}, {"offset", Total}});
    EXPECT_TRUE(End.getAsObject()->getArray("items")->empty());
    auto Reverse = Query(llvm::json::Object{{"side", Side},
                                            {"ambiguous_with", Anchor},
                                            {"sort", "address"},
                                            {"descending", true},
                                            {"limit", 1}});
    EXPECT_EQ(Reverse.getAsObject()
                  ->getArray("items")
                  ->front()
                  .getAsObject()
                  ->getString("address"),
              Addresses.back());
    auto Filtered = Query(llvm::json::Object{
        {"side", Side},
        {"ambiguous_with", Anchor},
        {"search", Secondary ? "after_copy_31" : "before_copy_31"}});
    EXPECT_EQ(Filtered.getAsObject()->getInteger("total"), 1);
    const auto Invalid = render(llvm::json::Object{
        {"side", Side}, {"ambiguous_with", Anchor}, {"offset", Total + 1}});
    EXPECT_EQ(neverd_comparison_functions_json(C, Invalid.c_str()), nullptr);
  }
  const auto Original = Query(
      llvm::json::Object{{"side", "secondary"}, {"ambiguous_with", "0x1010"}});
  EXPECT_EQ(neverd_comparison_functions_json(
                C, R"({"side":"secondary","ambiguous_with":"0xdead"})"),
            nullptr);
  EXPECT_EQ(neverd_comparison_functions_json(
                C, R"({"side":"secondary","ambiguous_with":4112})"),
            nullptr);
  ASSERT_EQ(
      neverd_comparison_override_json(
          C, R"({"kind":"match","primary":"0x1010","secondary":"0x8030"})"),
      1);
  auto Resolved = Query(
      llvm::json::Object{{"side", "secondary"}, {"ambiguous_with", "0x1010"}});
  EXPECT_EQ(Resolved.getAsObject()->getInteger("total"), 0);
  EXPECT_FALSE(Resolved.getAsObject()
                   ->getObject("ambiguity")
                   ->getBoolean("active")
                   .value_or(true));
  ASSERT_EQ(neverd_comparison_undo(C), 1);
  EXPECT_EQ(render(Query(llvm::json::Object{{"side", "secondary"},
                                            {"ambiguous_with", "0x1010"}})),
            render(Original));
  ASSERT_EQ(neverd_comparison_redo(C), 1);
  ASSERT_EQ(neverd_comparison_restore_json(C, Saved.c_str()), 1)
      << take(neverd_comparison_last_error(C));
  EXPECT_EQ(render(Query(llvm::json::Object{{"side", "secondary"},
                                            {"ambiguous_with", "0x1010"}})),
            render(Original));
}

TEST_F(BinaryDiffSDK,
       ResolvingLastCandidatesKeepsInventoryAndReportsConsistent) {
  for (const auto [PCount, SCount] :
       {std::pair(3u, 4u), std::pair(4u, 3u), std::pair(4u, 4u)}) {
    SCOPED_TRACE(PCount);
    SCOPED_TRACE(SCount);
    const auto Copies = [&](unsigned Count, uint64_t Base) {
      constexpr unsigned Stride = 16;
      std::vector<uint8_t> Bytes(Count * Stride, 0xcc);
      for (unsigned I = 0; I < Count; ++I) {
        const auto Code = body();
        std::copy(Code.begin(), Code.end(), Bytes.begin() + I * Stride);
      }
      const auto S = input(Bytes, Base);
      EXPECT_GE(neverd_session_discover_functions(S), Count);
      return S;
    };
    const auto P = Copies(PCount, 0x1000);
    const auto S = Copies(SCount, 0x8000);
    const char *Options = R"({"discover":false,"use_names":false})";
    const auto C = compare(P, S, Options), Reopened = compare(P, S, Options);
    const auto Initial = parsed(neverd_comparison_export_json(C));
    const auto *Groups = Initial.getAsObject()->getArray("ambiguities");
    ASSERT_TRUE(Groups);
    ASSERT_EQ(Groups->size(), 1u) << render(Initial);
    const auto &Group = *Groups->front().getAsObject();
    std::vector<std::string> Primary, Secondary;
    for (const auto &Address : *Group.getArray("primary"))
      Primary.push_back(Address.getAsString()->str());
    for (const auto &Address : *Group.getArray("secondary"))
      Secondary.push_back(Address.getAsString()->str());
    const size_t Pairs = std::min(Primary.size(), Secondary.size());
    ASSERT_GT(Pairs, 1u);
    std::vector<std::string> States{take(neverd_comparison_export_json(C))};
    for (size_t I = 0; I < Pairs; ++I) {
      const auto Edit = render(llvm::json::Object{{"kind", "match"},
                                                  {"primary", Primary[I]},
                                                  {"secondary", Secondary[I]}});
      ASSERT_EQ(neverd_comparison_override_json(C, Edit.c_str()), 1);
      States.push_back(take(neverd_comparison_export_json(C)));
      // Reopen every intermediate save in a separate comparison, preserving
      // the original's undo/redo history for the complete editing sequence.
      ASSERT_EQ(neverd_comparison_restore_json(Reopened, States.back().c_str()),
                1)
          << take(neverd_comparison_last_error(Reopened));
      EXPECT_EQ(take(neverd_comparison_export_json(Reopened)), States.back());
      const auto Summary = parsed(neverd_comparison_summary_json(C));
      EXPECT_EQ(
          Summary.getAsObject()->getObject("counts")->getInteger("ambiguities"),
          I + 1 < Pairs ? 1 : 0);
    }
    for (const bool IsSecondary : {false, true}) {
      const auto Side = IsSecondary ? "secondary" : "primary";
      const auto Inventory =
          render(llvm::json::Object{{"side", Side}, {"kind", "ambiguous"}});
      auto Page =
          parsed(neverd_comparison_functions_json(C, Inventory.c_str()));
      EXPECT_EQ(Page.getAsObject()->getInteger("total"), 0);
      const auto &Remaining = IsSecondary ? Primary : Secondary;
      if (Remaining.size() > Pairs) {
        const auto Query = render(llvm::json::Object{
            {"side", Side}, {"ambiguous_with", Remaining.back()}});
        Page = parsed(neverd_comparison_functions_json(C, Query.c_str()));
        EXPECT_EQ(Page.getAsObject()->getInteger("total"), 0);
        EXPECT_EQ(
            Page.getAsObject()->getObject("ambiguity")->getBoolean("active"),
            false);
      }
    }
    for (size_t I = Pairs; I > 0; --I) {
      ASSERT_EQ(neverd_comparison_undo(C), 1);
      EXPECT_EQ(take(neverd_comparison_export_json(C)), States[I - 1]);
    }
    for (size_t I = 0; I < Pairs; ++I) {
      ASSERT_EQ(neverd_comparison_redo(C), 1);
      EXPECT_EQ(take(neverd_comparison_export_json(C)), States[I + 1]);
    }
  }
}

TEST_F(BinaryDiffSDK, ScalarValidationBindsNativeLiftsAndExplicitInputDomain) {
  const auto A = input({0x89, 0xf8, 0x83, 0xc0, 3, 0xc3});
  const auto B =
      input({0x89, 0xf8, 0x83, 0xc0, 1, 0x83, 0xc0, 2, 0xc3}, 0x8000);
  const auto C = compare(
      A, B,
      R"({"discover":false,"overrides":[{"kind":"match","primary":"0x1000","secondary":"0x8000"}]})");
  const char *Query = R"({"side":"primary","address":"0x1000"})";
  auto Checked =
      parsed(neverd_comparison_scalar_validation_json(C, A, B, Query));
  auto *R = Checked.getAsObject();
  ASSERT_TRUE(R);
  EXPECT_EQ(R->getString("status"), "proved") << render(Checked);
  EXPECT_EQ(R->getString("input_domain"), "defined_bitvectors");
  EXPECT_EQ(R->getString("native_state_equivalence"), "not_proved");
  EXPECT_EQ(R->getBoolean("inputs_revalidated"), true);
  EXPECT_EQ(R->getBoolean("inputs_bound"), true);
  EXPECT_EQ(R->getString("input_source_binding"), "revalidated_sessions");
  EXPECT_EQ(neverd_comparison_owned_scalar_validation_json(C, Query), nullptr);
  EXPECT_EQ(R->getInteger("result_bits"), 32);
  ASSERT_TRUE(R->getArray("arguments"));
  ASSERT_EQ(R->getArray("arguments")->size(), 1U);
  EXPECT_EQ(R->getArray("arguments")
                ->front()
                .getAsObject()
                ->getInteger("register_offset"),
            56);
  EXPECT_EQ(R->getObject("primary_lift")->getBoolean("verified_native_body"),
            true);
  const auto Saved = take(neverd_comparison_export_json(C));
  EXPECT_EQ(Saved.find("scalar_validation"), std::string::npos);
  auto Reopened = compare(A, B);
  ASSERT_EQ(neverd_comparison_restore_json(Reopened, Saved.c_str()), 1);
  Checked =
      parsed(neverd_comparison_scalar_validation_json(Reopened, A, B, Query));
  EXPECT_EQ(Checked.getAsObject()->getString("status"), "proved");
  for (const auto *Bounds :
       {R"({"address":"0x1000","max_work":1})",
        R"({"address":"0x1000","max_native_instructions":1})"}) {
    auto Limited =
        parsed(neverd_comparison_scalar_validation_json(C, A, B, Bounds));
    EXPECT_EQ(Limited.getAsObject()->getString("state"), "resource_limited");
    EXPECT_EQ(Limited.getAsObject()->getString("status"), "budget_exceeded");
  }
  EXPECT_EQ(neverd_comparison_scalar_validation_json(
                C, A, B, R"({"address":"0x1000","max_work":0})"),
            nullptr);
  EXPECT_EQ(neverd_comparison_scalar_validation_json(C, nullptr, B, Query),
            nullptr);
  const auto Wrong = input({0x8d, 0x47, 5, 0xc3}, 0x8000);
  EXPECT_EQ(neverd_comparison_scalar_validation_json(C, A, Wrong, Query),
            nullptr);
  EXPECT_NE(take(neverd_comparison_last_error(C)).find("stale comparison"),
            std::string::npos);
  const auto WrongPrimary = input({0x8d, 0x47, 5, 0xc3});
  EXPECT_EQ(neverd_comparison_scalar_validation_json(C, WrongPrimary, B, Query),
            nullptr);
  EXPECT_NE(take(neverd_comparison_last_error(C)).find("stale comparison"),
            std::string::npos);
  auto Retried =
      parsed(neverd_comparison_scalar_validation_json(C, A, B, Query));
  EXPECT_EQ(Retried.getAsObject()->getString("status"), "proved");
}

TEST_F(BinaryDiffSDK, OwnedScalarInputsSurvivePathReplacementAndNeverReopenIt) {
  input({0x89, 0xf8, 0x83, 0xc0, 3, 0xc3});
  input({0x89, 0xf8, 0x83, 0xc0, 1, 0x83, 0xc0, 2, 0xc3}, 0x8000);
  const auto C = neverd_comparison_create();
  Comparisons.push_back(C);
  ASSERT_EQ(
      neverd_comparison_run_owned(
          C, &Sessions[0], &Sessions[1],
          R"({"discover":false,"overrides":[{"kind":"match","primary":"0x1000","secondary":"0x8000"}]})",
          nullptr, nullptr),
      1);
  EXPECT_EQ(Sessions[0], nullptr);
  EXPECT_EQ(Sessions[1], nullptr);
  const auto Summary = parsed(neverd_comparison_summary_json(C));
  EXPECT_EQ(Summary.getAsObject()->getBoolean("owned_validation_inputs"), true);
  const char *Query = R"({"address":"0x1000"})";
  const auto First =
      parsed(neverd_comparison_owned_scalar_validation_json(C, Query));
  const auto *R = First.getAsObject();
  ASSERT_TRUE(R);
  EXPECT_EQ(R->getString("status"), "proved") << render(First);
  EXPECT_EQ(R->getString("input_source_binding"), "owned_sessions");
  EXPECT_EQ(R->getBoolean("inputs_bound"), true);
  EXPECT_EQ(R->getBoolean("inputs_revalidated"), false);
  EXPECT_GT(R->getInteger("work").value_or(0), 0);
  const auto Saved = take(neverd_comparison_export_json(C));
  EXPECT_EQ(Saved.find("owned_sessions"), std::string::npos);
  EXPECT_EQ(Saved.find("scalar_validation"), std::string::npos);
  {
    std::ofstream Changed(Directory / "0.raw",
                          std::ios::binary | std::ios::trunc);
    const uint8_t Bytes[] = {0x89, 0xf8, 0x83, 0xc0, 5, 0xc3};
    Changed.write(reinterpret_cast<const char *>(Bytes), sizeof(Bytes));
  }
  std::filesystem::remove(Directory / "1.raw");
  EXPECT_EQ(take(neverd_comparison_owned_scalar_validation_json(C, Query)),
            render(First));
  const auto Changed = input({0x89, 0xf8, 0x83, 0xc0, 5, 0xc3});
  const auto Other = input({0x89, 0xf8, 0x83, 0xc0, 3, 0xc3}, 0x8000);
  EXPECT_EQ(neverd_comparison_scalar_validation_json(C, Changed, Other, Query),
            nullptr);
  EXPECT_NE(take(neverd_comparison_last_error(C)).find("stale comparison"),
            std::string::npos);
  EXPECT_EQ(take(neverd_comparison_owned_scalar_validation_json(C, Query)),
            render(First));
  ASSERT_EQ(neverd_comparison_restore_json(C, Saved.c_str()), 1);
  EXPECT_EQ(take(neverd_comparison_owned_scalar_validation_json(C, Query)),
            render(First));
}

TEST_F(BinaryDiffSDK, OwnedRunTransferIsExplicitOnAdmissionAndFailure) {
  input(body());
  input(body());
  const auto A = Sessions[0], B = Sessions[1];
  const auto C = neverd_comparison_create();
  Comparisons.push_back(C);
  EXPECT_EQ(neverd_comparison_run_owned(C, &Sessions[0], &Sessions[0], "{}",
                                        nullptr, nullptr),
            0);
  auto Alias = A;
  EXPECT_EQ(neverd_comparison_run_owned(C, &Sessions[0], &Alias, "{}", nullptr,
                                        nullptr),
            0);
  EXPECT_EQ(neverd_comparison_run_owned(C, nullptr, &Sessions[1], "{}", nullptr,
                                        nullptr),
            0);
  EXPECT_EQ(neverd_comparison_run_owned(nullptr, &Sessions[0], &Sessions[1],
                                        "{}", nullptr, nullptr),
            0);
  EXPECT_EQ(Sessions[0], A);
  EXPECT_EQ(Sessions[1], B);
  // Ownership admission succeeds before the options parser rejects the job.
  EXPECT_EQ(neverd_comparison_run_owned(C, &Sessions[0], &Sessions[1],
                                        R"({"unknown_option":true})", nullptr,
                                        nullptr),
            0);
  EXPECT_EQ(Sessions[0], nullptr);
  EXPECT_EQ(Sessions[1], nullptr);
  EXPECT_EQ(neverd_comparison_owned_scalar_validation_json(
                C, R"({"address":"0x1000"})"),
            nullptr);
  const auto NewA = input(body()), NewB = input(body());
  EXPECT_EQ(neverd_comparison_run_owned(C, &Sessions[2], &Sessions[3], "{}",
                                        nullptr, nullptr),
            0);
  EXPECT_EQ(Sessions[2], NewA);
  EXPECT_EQ(Sessions[3], NewB);
  const auto D = neverd_comparison_create();
  Comparisons.push_back(D);
  EXPECT_EQ(neverd_comparison_run_owned(D, &Sessions[2], &Sessions[2], "{}",
                                        nullptr, nullptr),
            0);
  EXPECT_FALSE(take(neverd_comparison_last_error(D)).empty());
  ASSERT_EQ(neverd_comparison_run_owned(D, &Sessions[2], &Sessions[3],
                                        R"({"discover":false})", nullptr,
                                        nullptr),
            1);
  EXPECT_EQ(Sessions[2], nullptr);
  EXPECT_EQ(Sessions[3], nullptr);
  EXPECT_TRUE(take(neverd_comparison_last_error(D)).empty());
  const auto Status = parsed(neverd_comparison_status_json(D));
  EXPECT_EQ(Status.getAsObject()->getString("error"), "");
}

TEST_F(BinaryDiffSDK, OwnedValidationRequiresCompleteNativeCapture) {
  input(body());
  input(body());
  const auto C = neverd_comparison_create();
  Comparisons.push_back(C);
  ASSERT_EQ(neverd_comparison_run_owned(
                C, &Sessions[0], &Sessions[1],
                R"({"discover":false,"max_instructions":1})", nullptr, nullptr),
            1);
  auto Summary = parsed(neverd_comparison_summary_json(C));
  EXPECT_EQ(Summary.getAsObject()->getBoolean("owned_validation_inputs"),
            false);
  EXPECT_EQ(neverd_comparison_owned_scalar_validation_json(
                C, R"({"address":"0x1000"})"),
            nullptr);
  EXPECT_NE(
      take(neverd_comparison_last_error(C)).find("completed native capture"),
      std::string::npos);
}

TEST_F(BinaryDiffSDK, OwnedScalarQueryCanCancelTimeOutAndRetryFreshLifting) {
  std::vector<uint8_t> Bytes{0x89, 0xf8}; // mov eax, edi
  Bytes.insert(Bytes.end(), 8000, 0x90);
  Bytes.insert(Bytes.end(), {0x83, 0xc0, 3, 0xc3}); // add eax, 3; ret
  input(Bytes);
  input(Bytes);
  const auto C = neverd_comparison_create();
  Comparisons.push_back(C);
  ASSERT_EQ(neverd_comparison_run_owned(C, &Sessions[0], &Sessions[1],
                                        R"({"discover":false})", nullptr,
                                        nullptr),
            1);
  const char *Query = R"({"address":"0x1000","max_native_instructions":16384})";
  auto Work = std::async(std::launch::async, [&] {
    return take(neverd_comparison_owned_scalar_validation_json(C, Query));
  });
  bool CancelledDuringLift = false;
  while (Work.wait_for(std::chrono::milliseconds(0)) !=
         std::future_status::ready) {
    const auto Status = parsed(neverd_comparison_status_json(C));
    if (Status.getAsObject()->getString("phase") == "validation:primary_lift") {
      neverd_comparison_cancel(C);
      CancelledDuringLift = true;
      break;
    }
    std::this_thread::yield();
  }
  auto Stopped = llvm::json::parse(Work.get());
  ASSERT_TRUE(static_cast<bool>(Stopped));
  ASSERT_TRUE(CancelledDuringLift);
  EXPECT_EQ(Stopped->getAsObject()->getString("state"), "cancelled");
  EXPECT_EQ(Stopped->getAsObject()->getString("status"), "cancelled");
  const auto Again =
      parsed(neverd_comparison_owned_scalar_validation_json(C, Query));
  EXPECT_EQ(Again.getAsObject()->getString("status"), "proved")
      << render(Again);
  const auto Timed = parsed(neverd_comparison_owned_scalar_validation_json(
      C,
      R"({"address":"0x1000","max_native_instructions":16384,"timeout_ms":1})"));
  EXPECT_EQ(Timed.getAsObject()->getString("state"), "timed_out")
      << render(Timed);
  EXPECT_EQ(Timed.getAsObject()->getString("status"), "cancelled");
  EXPECT_EQ(take(neverd_comparison_owned_scalar_validation_json(C, Query)),
            render(Again));
}

TEST_F(BinaryDiffSDK, ScalarValidationCanStopRebindingAndRetry) {
  std::vector<uint8_t> Bytes(60000, 0x90);
  Bytes.push_back(0xc3);
  const auto A = input(Bytes);
  const auto C = compare(A, A);
  auto Future = std::async(std::launch::async, [&] {
    return take(neverd_comparison_scalar_validation_json(
        C, A, A, R"({"address":"0x1000"})"));
  });
  bool Cancelled = false;
  while (Future.wait_for(std::chrono::milliseconds(0)) !=
         std::future_status::ready) {
    auto S = parsed(neverd_comparison_status_json(C));
    if (S.getAsObject()->getBoolean("running").value_or(false) &&
        S.getAsObject()->getString("phase").value_or("").starts_with(
            "validation:")) {
      neverd_comparison_cancel(C);
      Cancelled = true;
      break;
    }
    std::this_thread::yield();
  }
  const auto Text = Future.get();
  ASSERT_TRUE(Cancelled);
  auto Stopped = llvm::json::parse(Text);
  ASSERT_TRUE(static_cast<bool>(Stopped));
  EXPECT_EQ(Stopped->getAsObject()->getString("state"), "cancelled");
  EXPECT_NE(Stopped->getAsObject()->getString("status"), "proved");
  const auto Retry = parsed(neverd_comparison_scalar_validation_json(
      C, A, A, R"({"address":"0x1000","max_native_instructions":1})"));
  EXPECT_EQ(Retry.getAsObject()->getString("state"), "resource_limited");
  EXPECT_EQ(Retry.getAsObject()->getBoolean("inputs_revalidated"), true);
  const auto Expired = parsed(neverd_comparison_scalar_validation_json(
      C, A, A, R"({"address":"0x1000","timeout_ms":1})"));
  EXPECT_EQ(Expired.getAsObject()->getString("state"), "timed_out");
  EXPECT_EQ(Expired.getAsObject()->getBoolean("inputs_revalidated"), false);
}

TEST_F(BinaryDiffSDK, ScalarValidationRefusesDifferentBindingsAndMemory) {
  const auto A = input({0x89, 0xf8, 0x83, 0xc0, 3, 0xc3});
  for (const auto &Bytes :
       {std::vector<uint8_t>{0x89, 0xf8, 0x83, 0xc0, 5,
                             0xc3}, // Changed return.
        std::vector<uint8_t>{0x89, 0xf0, 0x83, 0xc0, 3, 0xc3}, // Different GPR.
        std::vector<uint8_t>{0x8d, 0x47, 3,
                             0xc3}, // 64-bit input, 32-bit result.
        std::vector<uint8_t>{0x8b, 0x07, 0xc3}}) { // Memory read.
    const auto B = input(Bytes, 0x8000);
    const auto C = compare(
        A, B,
        R"({"discover":false,"overrides":[{"kind":"match","primary":"0x1000","secondary":"0x8000"}]})");
    auto R = parsed(neverd_comparison_scalar_validation_json(
        C, A, B, R"({"address":"0x1000"})"));
    EXPECT_NE(R.getAsObject()->getString("status"), "proved") << render(R);
    EXPECT_EQ(R.getAsObject()->getBoolean("unproved_means_inequivalent"),
              false);
    EXPECT_EQ(R.getAsObject()->getString("security_assessment"),
              "not_performed");
    if (Bytes.size() == 6 && Bytes[4] == 5)
      EXPECT_EQ(R.getAsObject()->getString("status"), "unproved") << render(R);
    else
      EXPECT_EQ(R.getAsObject()->getString("status"), "unsupported")
          << render(R);
  }
}

TEST_F(BinaryDiffSDK,
       IdenticalNativeInputsKeepOriginalEvidenceAndOutliveSessions) {
  auto A = input(body());
  auto B = input(body());
  auto C = compare(A, B);
  neverd_session_destroy(A);
  Sessions[0] = nullptr;
  neverd_session_destroy(B);
  Sessions[1] = nullptr;
  auto Document = parsed(neverd_comparison_export_json(C));
  auto *Root = Document.getAsObject();
  ASSERT_NE(Root, nullptr);
  EXPECT_EQ(Root->getString("state"), "complete");
  auto *Matches = Root->getArray("matches");
  ASSERT_NE(Matches, nullptr);
  ASSERT_EQ(Matches->size(), 1u);
  auto *M = (*Matches)[0].getAsObject();
  ASSERT_NE(M, nullptr);
  EXPECT_TRUE(M->getBoolean("analysis_complete").value_or(false));
  ASSERT_NE(M->getArray("changes"), nullptr);
  EXPECT_TRUE(M->getArray("changes")->empty());
  auto Detail = parsed(neverd_comparison_function_json(C, 0, 0x1000));
  auto *D = Detail.getAsObject();
  ASSERT_NE(D, nullptr);
  auto *Rows = D->getArray("rows");
  ASSERT_NE(Rows, nullptr);
  ASSERT_EQ(Rows->size(), 3u);
  auto *First = (*Rows)[0].getAsObject()->getObject("primary");
  ASSERT_NE(First, nullptr);
  EXPECT_EQ(First->getString("mnemonic"), "mov");
  EXPECT_EQ(First->getString("bytes"), "B807000000");
  EXPECT_EQ(First->getString("operands"), "eax, 7");
  EXPECT_TRUE(D->getObject("evidence")
                  ->getBoolean("normalized_unchanged")
                  .value_or(false));
}

TEST_F(BinaryDiffSDK, LongNativeCallContextSurvivesReorderingAndRestore) {
  const auto Make = [](bool ReverseHelpers) {
    std::vector<uint8_t> Bytes;
    const auto Call = [&](uint32_t Target) {
      const uint32_t Relative = Target - uint32_t(Bytes.size() + 5);
      Bytes.push_back(0xe8);
      for (unsigned Shift = 0; Shift < 32; Shift += 8)
        Bytes.push_back(uint8_t(Relative >> Shift));
    };
    Call(ReverseHelpers ? 0x2010 : 0x2000);
    for (unsigned I = 0; I < 1200; ++I)
      Call(0x2020);
    Call(ReverseHelpers ? 0x2000 : 0x2010);
    Bytes.push_back(0xc3);
    Bytes.resize(0x2030, 0xcc);
    for (unsigned Offset : {0x2000u, 0x2010u, 0x2020u}) {
      const auto Code = body(Offset == 0x2020 ? 29 : 7);
      std::copy(Code.begin(), Code.end(), Bytes.begin() + Offset);
    }
    return Bytes;
  };
  const auto A = input(Make(false));
  const auto B = input(Make(true), 0x8000);
  ASSERT_GE(neverd_session_discover_functions(A), 4u);
  ASSERT_GE(neverd_session_discover_functions(B), 4u);
  const auto C = compare(A, B, R"({"discover":false,"use_names":false})");
  const auto Saved = take(neverd_comparison_export_json(C));
  const auto Document = parsed(neverd_comparison_export_json(C));
  ASSERT_TRUE(Document.getAsObject());
  EXPECT_EQ(Document.getAsObject()->getInteger("matcher_version"), 4);
  const auto *Matches = Document.getAsObject()->getArray("matches");
  ASSERT_TRUE(Matches);
  const std::map<std::string, std::string> Expected = {{"0x3000", "0xa010"},
                                                       {"0x3010", "0xa000"}};
  unsigned Found = 0;
  for (const auto &Match : *Matches) {
    const auto *M = Match.getAsObject();
    ASSERT_TRUE(M);
    const auto Address = M->getString("primary");
    ASSERT_TRUE(Address);
    if (const auto It = Expected.find(Address->str()); It != Expected.end()) {
      EXPECT_EQ(M->getString("secondary"), It->second);
      EXPECT_EQ(M->getString("step"), "call_site");
      ++Found;
    }
  }
  EXPECT_EQ(Found, 2u);
  const auto Detail = parsed(neverd_comparison_function_json(C, 0, 0x1000));
  ASSERT_TRUE(Detail.getAsObject());
  const auto *Rows = Detail.getAsObject()->getArray("rows");
  ASSERT_TRUE(Rows);
  EXPECT_EQ(Rows->size(), 1203u);
  const auto Page = parsed(neverd_comparison_function_page_json(
      C, R"({"address":"0x1000","offset":1198,"limit":5})"));
  ASSERT_TRUE(Page.getAsObject());
  const auto *PageRows = Page.getAsObject()->getArray("rows");
  ASSERT_TRUE(PageRows);
  ASSERT_EQ(PageRows->size(), 5u);
  for (size_t I = 0; I < PageRows->size(); ++I)
    EXPECT_EQ((*PageRows)[I], (*Rows)[1198 + I]);
  ASSERT_EQ(neverd_comparison_restore_json(C, Saved.c_str()), 1)
      << take(neverd_comparison_last_error(C));
  EXPECT_EQ(take(neverd_comparison_export_json(C)), Saved);
  EXPECT_EQ(render(parsed(neverd_comparison_function_json(C, 0, 0x1000))),
            render(Detail));
}

TEST_F(BinaryDiffSDK, LongNativeInsertionsKeepFullPagedAndSavedEvidence) {
  const auto Make = [](bool Insert) {
    std::vector<uint8_t> Code;
    const auto Add = [&](uint32_t Immediate) {
      Code.push_back(0x05); // add eax, imm32
      for (unsigned Shift = 0; Shift < 32; Shift += 8)
        Code.push_back(uint8_t(Immediate >> Shift));
    };
    for (uint32_t I = 0; I < 6000; ++I) {
      if (Insert && I == 1000)
        for (unsigned J = 0; J < 40; ++J)
          Add(0x1234567);
      Add(0x800000 + I);
    }
    Code.push_back(0xc3);
    return Code;
  };
  auto C = compare(input(Make(false)), input(Make(true)));
  const auto Saved = take(neverd_comparison_export_json(C));
  const auto Document = parsed(neverd_comparison_export_json(C));
  const auto *Matches = Document.getAsObject()->getArray("matches");
  ASSERT_TRUE(Matches);
  ASSERT_EQ(Matches->size(), 1u);
  ASSERT_EQ(Matches->front().getAsObject()->getInteger("equal_instructions"),
            6001);
  auto Full = parsed(neverd_comparison_function_json(C, 0, 0x1000));
  ASSERT_TRUE(Full.getAsObject());
  EXPECT_TRUE(
      Full.getAsObject()->getBoolean("computation_complete").value_or(false));
  const auto *Rows = Full.getAsObject()->getArray("rows");
  ASSERT_TRUE(Rows);
  ASSERT_EQ(Rows->size(), 6041u);
  unsigned Original = 0, Inserted = 0;
  for (const auto &Row : *Rows) {
    const auto *O = Row.getAsObject();
    ASSERT_TRUE(O);
    if (O->getString("kind") == "inserted") {
      EXPECT_FALSE(O->getObject("primary"));
      ASSERT_TRUE(O->getObject("secondary"));
      ++Inserted;
    } else {
      EXPECT_EQ(O->getString("kind"), "identical");
      ASSERT_TRUE(O->getObject("primary"));
      ASSERT_TRUE(O->getObject("secondary"));
      EXPECT_EQ(O->getObject("primary")->getString("bytes"),
                O->getObject("secondary")->getString("bytes"));
      ++Original;
    }
  }
  EXPECT_EQ(Original, 6001u);
  EXPECT_EQ(Inserted, 40u);
  auto Page = parsed(neverd_comparison_function_page_json(
      C, R"({"address":"0x1000","offset":980,"limit":80})"));
  ASSERT_TRUE(Page.getAsObject());
  EXPECT_EQ(Page.getAsObject()->getInteger("total"), 6041);
  const auto *PageRows = Page.getAsObject()->getArray("rows");
  ASSERT_TRUE(PageRows);
  ASSERT_EQ(PageRows->size(), 80u);
  for (size_t I = 0; I < PageRows->size(); ++I)
    EXPECT_EQ((*PageRows)[I], (*Rows)[980 + I]);
  ASSERT_EQ(neverd_comparison_restore_json(C, Saved.c_str()), 1)
      << take(neverd_comparison_last_error(C));
  EXPECT_EQ(take(neverd_comparison_export_json(C)), Saved);
  EXPECT_EQ(render(parsed(neverd_comparison_function_json(C, 0, 0x1000))),
            render(Full));
}

TEST_F(BinaryDiffSDK, SameSizeImmediateEditIsNotHiddenBySimilarStructure) {
  auto C = compare(input(body()), input(body(9)),
                   "{\"discover\":false,\"use_names\":false}");
  auto Detail = parsed(neverd_comparison_function_json(C, 0, 0x1000));
  const auto *D = Detail.getAsObject();
  ASSERT_NE(D, nullptr);
  const auto *E = D->getObject("evidence");
  ASSERT_NE(E, nullptr);
  EXPECT_FALSE(E->getBoolean("normalized_unchanged").value_or(true));
  const auto *Rows = D->getArray("rows");
  ASSERT_NE(Rows, nullptr);
  bool Constant = false;
  for (const auto &R : *Rows)
    Constant |= R.getAsObject()->getString("kind") == "constant";
  EXPECT_TRUE(Constant);
}

TEST_F(BinaryDiffSDK,
       CallImpactRetainsOriginalWitnessesUnknownsAndReopenParity) {
  // entry: call 0x1010; call rax; ret. Callee is authored, never executed.
  std::vector<uint8_t> Bytes{0xe8, 0x0b, 0, 0, 0, 0xff, 0xd0, 0xc3};
  Bytes.resize(16, 0xcc);
  const auto Body = body();
  Bytes.insert(Bytes.end(), Body.begin(), Body.end());
  auto A = input(Bytes), B = input(Bytes, 0x8000);
  auto C = compare(A, B, R"({"discover":true})");
  const auto Saved = take(neverd_comparison_export_json(C));
  const char *Query = R"({"seeds":["0x1010"],"limit":1})";
  auto Page = parsed(neverd_comparison_impact_json(C, Query));
  auto *O = Page.getAsObject();
  ASSERT_NE(O, nullptr);
  ASSERT_EQ(O->getInteger("total"), 2);
  ASSERT_EQ(O->getArray("rows")->size(), 1u);
  EXPECT_EQ(O->getString("state"), "complete");
  EXPECT_EQ(O->getString("assessment"), "possible_call_reachability");
  EXPECT_EQ(O->getString("parameter_return_binding"), "not_modeled");
  EXPECT_EQ(O->getString("semantic_validation"), "not_performed");
  EXPECT_EQ(O->getInteger("boundary_count"), 1);
  auto Next = parsed(neverd_comparison_impact_json(
      C, R"({"seeds":["0x1010"],"offset":1,"limit":1})"));
  const auto *Rows = Next.getAsObject()->getArray("rows");
  ASSERT_EQ(Rows->size(), 1u);
  const auto *Caller = (*Rows)[0].getAsObject();
  EXPECT_EQ(Caller->getString("address"), "0x1000");
  EXPECT_EQ(Caller->getInteger("depth"), 1);
  ASSERT_NE(Caller->getObject("witness"), nullptr);
  EXPECT_EQ(Caller->getObject("witness")
                ->getObject("instruction")
                ->getString("bytes"),
            "E80B000000");
  auto Path = parsed(neverd_comparison_impact_json(
      C, R"({"seeds":["0x1010"],"kind":"path","witness":1})"));
  ASSERT_EQ(Path.getAsObject()->getInteger("total"), 1);
  EXPECT_EQ(Path.getAsObject()->getInteger("work"), O->getInteger("work"));
  auto Entries = parsed(neverd_comparison_impact_json(
      C, R"({"seeds":["0x1010"],"kind":"entrypoints"})"));
  ASSERT_EQ(Entries.getAsObject()->getInteger("total"), 1);
  const auto *Entry =
      (*Entries.getAsObject()->getArray("rows"))[0].getAsObject();
  EXPECT_EQ(Entry->getString("address"), "0x1000");
  EXPECT_TRUE(Entry->getBoolean("entry_point").value_or(false));
  auto Boundary = parsed(neverd_comparison_impact_json(
      C, R"({"seeds":["0x1010"],"kind":"boundaries"})"));
  ASSERT_EQ(Boundary.getAsObject()->getInteger("total"), 1);
  EXPECT_EQ((*Boundary.getAsObject()->getArray("rows"))[0]
                .getAsObject()
                ->getObject("instruction")
                ->getString("address"),
            "0x1005");
  auto Limited = parsed(neverd_comparison_impact_json(
      C, R"({"seeds":["0x1010"],"max_depth":0,"kind":"frontier"})"));
  EXPECT_EQ(Limited.getAsObject()->getString("state"), "resource_limited");
  EXPECT_EQ(Limited.getAsObject()->getInteger("total"), 1);
  const char *EffectsQuery =
      R"({"seeds":["0x1010"],"kind":"effects","limit":1})";
  const auto Effects = parsed(neverd_comparison_impact_json(C, EffectsQuery));
  const auto *EO = Effects.getAsObject();
  ASSERT_NE(EO, nullptr);
  ASSERT_NE(EO->getObject("effect_review"), nullptr);
  EXPECT_EQ(EO->getInteger("total"), 2);
  EXPECT_EQ(EO->getObject("effect_review")->getString("profile"),
            "native-import-effects-v1");
  EXPECT_EQ(EO->getObject("effect_review")->getInteger("qualified_contracts"),
            0);
  EXPECT_EQ(
      (*EO->getArray("rows"))[0].getAsObject()->getObject("effect")->getString(
          "status"),
      "internal_target");
  auto UnknownEffect = parsed(neverd_comparison_impact_json(
      C, R"({"seeds":["0x1010"],"kind":"effects","offset":1,"limit":1})"));
  EXPECT_EQ((*UnknownEffect.getAsObject()->getArray("rows"))[0]
                .getAsObject()
                ->getObject("effect")
                ->getString("status"),
            "unresolved_target");
  EXPECT_EQ(UnknownEffect.getAsObject()
                ->getObject("effect_review")
                ->getInteger("work"),
            EO->getObject("effect_review")->getInteger("work"));
  auto EffectStop = parsed(neverd_comparison_impact_json(
      C, R"({"seeds":["0x1010"],"kind":"effects","max_effect_work":1})"));
  EXPECT_EQ(EffectStop.getAsObject()->getString("state"), "resource_limited");
  EXPECT_TRUE(
      EffectStop.getAsObject()->getBoolean("index_complete").value_or(false));
  EXPECT_FALSE(EffectStop.getAsObject()
                   ->getObject("effect_review")
                   ->getBoolean("inspection_complete")
                   .value_or(true));
  for (const char *Invalid :
       {R"({"seeds":[]})", R"({"seeds":["0x1001"]})", R"({"seeds":[4096]})",
        R"({"seeds":["0x1010"],"max_work":0})",
        R"({"seeds":["0x1010"],"witness":1})",
        R"({"seeds":["0x1010"],"kind":"path","witness":99})",
        R"({"seeds":["0x1010"],"max_effect_records":1})",
        R"({"seeds":["0x1010"],"kind":"effects","max_effect_records":0})",
        R"({"seeds":["0x1010"],"direction":"backward"})"})
    EXPECT_EQ(neverd_comparison_impact_json(C, Invalid), nullptr);
  EXPECT_EQ(neverd_comparison_restore_json(C, Saved.c_str()), 1);
  EXPECT_EQ(render(parsed(neverd_comparison_impact_json(C, EffectsQuery))),
            render(Effects));
  EXPECT_EQ(render(parsed(neverd_comparison_impact_json(C, Query))),
            render(Page));
  neverd_session_destroy(A);
  neverd_session_destroy(B);
  Sessions[0] = Sessions[1] = nullptr;
  auto Secondary = parsed(neverd_comparison_impact_json(
      C, R"({"side":"secondary","seeds":["0x8010"],"kind":"sites"})"));
  ASSERT_EQ(Secondary.getAsObject()->getInteger("total"), 1);
  EXPECT_EQ((*Secondary.getAsObject()->getArray("rows"))[0]
                .getAsObject()
                ->getObject("instruction")
                ->getString("address"),
            "0x8000");
  EXPECT_EQ(take(neverd_comparison_export_json(C)), Saved);
}

TEST_F(BinaryDiffSDK, ScalarCoincidingWithImageBaseDoesNotNormalizeAsAddress) {
  auto Left = body();
  Left[1] = 0;
  Left[2] = 0x10;
  auto Right = Left;
  Right[2] = 0x20;
  auto C = compare(input(Left, 0x1000), input(Right, 0x2000),
                   "{\"discover\":false,\"use_names\":false}");
  auto D = parsed(neverd_comparison_function_json(C, 0, 0x1000));
  ASSERT_NE(D.getAsObject()->getObject("evidence"), nullptr);
  EXPECT_FALSE(D.getAsObject()
                   ->getObject("evidence")
                   ->getBoolean("normalized_unchanged")
                   .value_or(true));
}

TEST_F(BinaryDiffSDK, RebasedBranchBodyRetainsCFGAndCorrespondence) {
  // cmp edi, 1; je second; mov eax, 2; ret; second: mov eax, 3; ret
  const std::vector<uint8_t> Code{0x83, 0xff, 1,    0x74, 6, 0xb8, 2, 0,   0,
                                  0,    0xc3, 0xb8, 3,    0, 0,    0, 0xc3};
  auto C = compare(input(Code, 0x1000), input(Code, 0x8000),
                   "{\"discover\":false,\"use_names\":false}");
  auto D = parsed(neverd_comparison_function_json(C, 0, 0x1000));
  const auto *O = D.getAsObject();
  ASSERT_NE(O->getObject("evidence"), nullptr);
  EXPECT_TRUE(O->getObject("evidence")
                  ->getBoolean("normalized_unchanged")
                  .value_or(false));
  EXPECT_EQ(O->getObject("primary")->getArray("blocks")->size(), 3u);
  EXPECT_EQ(O->getObject("primary")->getArray("edges")->size(), 2u);
  EXPECT_EQ(
      O->getObject("secondary")->getObject("function")->getString("address"),
      "0x8000");
}

TEST_F(BinaryDiffSDK, TruncatedTailIsVisibleEvenWhenInputsAreIdentical) {
  auto Code = body();
  Code.pop_back();
  Code.push_back(0x0f);
  auto C = compare(input(Code), input(Code));
  auto D = parsed(neverd_comparison_function_json(C, 0, 0x1000));
  auto *O = D.getAsObject();
  EXPECT_TRUE(O->getObject("primary")
                  ->getObject("function")
                  ->getBoolean("incomplete")
                  .value_or(false));
  ASSERT_NE(O->getObject("evidence"), nullptr);
  EXPECT_FALSE(O->getObject("evidence")
                   ->getBoolean("normalized_unchanged")
                   .value_or(true));
  auto Summary = parsed(neverd_comparison_summary_json(C));
  const auto *Counts = Summary.getAsObject()->getObject("counts");
  ASSERT_NE(Counts, nullptr);
  EXPECT_EQ(Counts->getInteger("changed"), 0);
  EXPECT_EQ(Counts->getInteger("normalized_unchanged"), 0);
  EXPECT_EQ(Counts->getInteger("incomplete_without_observed_change"), 1);
  auto Changed = parsed(
      neverd_comparison_functions_json(C, R"({"kind":"changed","limit":1})"));
  EXPECT_EQ(Changed.getAsObject()->getInteger("total"), 0);
}

TEST_F(BinaryDiffSDK,
       SaveRestoreEditsUndoAndRevisionRejectionAreTransactional) {
  auto A = input(body()), B = input(body(9));
  auto C = compare(A, B);
  const auto Saved = take(neverd_comparison_export_json(C));
  ASSERT_EQ(
      neverd_comparison_override_json(
          C, R"({"kind":"unmatch","primary":"0x1000","secondary":"0x1000"})"),
      1);
  auto Unmatched = parsed(
      neverd_comparison_functions_json(C, R"({"kind":"unmatched","limit":1})"));
  EXPECT_EQ(Unmatched.getAsObject()->getInteger("total"), 1);
  ASSERT_EQ(neverd_comparison_undo(C), 1);
  EXPECT_EQ(take(neverd_comparison_export_json(C)), Saved);
  ASSERT_EQ(neverd_comparison_redo(C), 1);
  ASSERT_EQ(neverd_comparison_restore_json(C, Saved.c_str()), 1);
  EXPECT_EQ(take(neverd_comparison_export_json(C)), Saved);
  auto Stale = llvm::json::parse(Saved);
  ASSERT_TRUE(bool(Stale));
  auto *Primary = Stale->getAsObject()->getObject("primary");
  ASSERT_NE(Primary, nullptr);
  (*Primary)["revision"] = "other-analysis";
  EXPECT_EQ(neverd_comparison_restore_json(C, render(*Stale).c_str()), 0);
  EXPECT_EQ(take(neverd_comparison_export_json(C)), Saved);
  EXPECT_EQ(neverd_comparison_validate_inputs(C, A, B), 1);
  EXPECT_EQ(neverd_comparison_validate_inputs(C, B, A), 0);
  auto Reopened = compare(A, B);
  EXPECT_EQ(neverd_comparison_restore_json(Reopened, Saved.c_str()), 1);
}

TEST_F(BinaryDiffSDK, SavedFactsCannotHideAChangedOrIncompleteBody) {
  auto Check = [&](neverd_comparison_t C, auto Mutate) {
    const auto Saved = take(neverd_comparison_export_json(C));
    auto Forged = llvm::json::parse(Saved);
    ASSERT_TRUE(bool(Forged));
    auto *Match =
        (*Forged->getAsObject()->getArray("matches"))[0].getAsObject();
    ASSERT_NE(Match, nullptr);
    Mutate(*Match);
    EXPECT_EQ(neverd_comparison_restore_json(C, render(*Forged).c_str()), 0);
    EXPECT_EQ(take(neverd_comparison_export_json(C)), Saved);
  };
  auto C = compare(input(body()), input(body(9)));
  Check(C, [](auto &M) {
    M["changes"] = llvm::json::Array{};
    M["similarity"] = 1.0;
  });
  Check(C, [](auto &M) { M["identity_only"] = true; });
  Check(C, [](auto &M) { M["equal_instructions"] = 999; });
  auto Code = body();
  Code.pop_back();
  Code.push_back(0x0f);
  auto Partial = compare(input(Code), input(Code));
  Check(Partial, [](auto &M) { M["analysis_complete"] = true; });
}

TEST_F(BinaryDiffSDK, InvalidContractsAndUnsupportedProfilesFailWithoutResult) {
  auto A = input(body()), B = input(body());
  for (const char *Options :
       {R"({"profile":"cross-isa"})", R"({"discover":1})",
        R"({"min_similarity":2})", R"({"max_candidates":0})", R"({"typo":1})",
        R"({"primary":{"begin":"0x20","end":"0x10"}})"}) {
    auto C = neverd_comparison_create();
    Comparisons.push_back(C);
    EXPECT_EQ(neverd_comparison_run(C, A, B, Options, nullptr, nullptr), 0)
        << Options;
    EXPECT_FALSE(take(neverd_comparison_last_error(C)).empty());
    auto Status = parsed(neverd_comparison_status_json(C));
    EXPECT_FALSE(
        Status.getAsObject()->getBoolean("result_available").value_or(true));
    EXPECT_EQ(Status.getAsObject()->getString("state"), "failed");
  }
  auto C = compare(A, B);
  EXPECT_EQ(neverd_comparison_functions_json(C, R"({"limit":0})"), nullptr);
  EXPECT_EQ(neverd_comparison_functions_json(C, R"({"side":"invalid"})"),
            nullptr);
  EXPECT_EQ(neverd_comparison_restore_json(C, "[]"), 0);
}

TEST_F(BinaryDiffSDK, CallEffectInspectionCanCancelCacheAndRefresh) {
  std::vector<uint8_t> Code(60000, 0x90);
  Code.insert(Code.end(), {0xff, 0xd0, 0xc3});
  auto C = compare(input(Code), input(Code));
  const char *Query = R"({"seeds":["0x1000"],"kind":"effects","limit":1})";
  const auto Text = cancelRunningInspection(C, "impact:effects", [&] {
    return neverd_comparison_impact_json(
        C, R"({"seeds":["0x1000"],"kind":"effects","limit":1,"refresh":true})");
  });
  auto Stopped = llvm::json::parse(Text);
  ASSERT_TRUE(bool(Stopped));
  const auto *O = Stopped->getAsObject();
  ASSERT_NE(O, nullptr);
  EXPECT_EQ(O->getString("state"), "cancelled");
  EXPECT_TRUE(O->getBoolean("index_complete").value_or(false));
  EXPECT_EQ(O->getObject("effect_review")->getString("state"), "cancelled");
  EXPECT_EQ(take(neverd_comparison_impact_json(C, Query)), Text);
  const auto Fresh = parsed(neverd_comparison_impact_json(
      C, R"({"seeds":["0x1000"],"kind":"effects","limit":1,"refresh":true})"));
  EXPECT_EQ(Fresh.getAsObject()->getString("state"), "complete");
  EXPECT_EQ(Fresh.getAsObject()->getInteger("total"), 1);
  const auto *Effect =
      (*Fresh.getAsObject()->getArray("rows"))[0].getAsObject()->getObject(
          "effect");
  EXPECT_EQ(Effect->getString("status"), "unresolved_target");
  ASSERT_NE(Effect->get("release_may_fail"), nullptr);
  EXPECT_EQ(Effect->get("release_may_fail")->kind(), llvm::json::Value::Null);
  auto Timeout = parsed(neverd_comparison_impact_json(
      C,
      R"({"seeds":["0x1000"],"kind":"effects","timeout_ms":1,"refresh":true})"));
  EXPECT_EQ(Timeout.getAsObject()->getString("state"), "timed_out");
}

TEST_F(BinaryDiffSDK, FunctionDetailCancellationCacheAndRefreshKeepState) {
  // A large, owned straight-line body keeps the real alignment operation
  // running long enough to exercise concurrent status/cancel, without target
  // execution or test-only hooks in the implementation.
  std::vector<uint8_t> Code(60000, 0x90);
  Code.push_back(0xc3);
  auto C = compare(input(Code), input(Code));
  const auto Text = cancelRunningInspection(C, "detail:compare", [&] {
    return neverd_comparison_function_page_json(
        C, R"({"address":"0x1000","refresh":true,"limit":1})");
  });
  auto Stopped = llvm::json::parse(Text);
  ASSERT_TRUE(bool(Stopped));
  const auto *O = Stopped->getAsObject();
  ASSERT_NE(O, nullptr);
  EXPECT_EQ(O->getString("state"), "cancelled");
  EXPECT_FALSE(O->getBoolean("computation_complete").value_or(true));
  ASSERT_NE(O->getArray("rows"), nullptr);
  EXPECT_TRUE(O->getArray("rows")->empty());
  const auto Cached = take(neverd_comparison_function_page_json(
      C, R"({"address":"0x1000","limit":1})"));
  EXPECT_EQ(Cached, Text);
  auto LaterPage = parsed(neverd_comparison_function_page_json(
      C, R"({"address":"0x1000","offset":200,"limit":1})"));
  EXPECT_EQ(LaterPage.getAsObject()->getString("state"), "cancelled");
  auto Graph =
      parsed(neverd_comparison_graph_json(C, R"({"address":"0x1000"})"));
  EXPECT_EQ(Graph.getAsObject()->getString("state"), "cancelled");
  auto *Nodes = Graph.getAsObject()->getArray("nodes");
  ASSERT_NE(Nodes, nullptr);
  ASSERT_FALSE(Nodes->empty());
  for (const auto &Node : *Nodes)
    EXPECT_EQ(Node.getAsObject()->getObject("comparison")->getString("state"),
              "not_compared");
  auto Fresh = parsed(neverd_comparison_function_page_json(
      C, R"({"address":"0x1000","refresh":true,"limit":1})"));
  EXPECT_EQ(Fresh.getAsObject()->getString("state"), "complete");
  EXPECT_TRUE(
      Fresh.getAsObject()->getBoolean("computation_complete").value_or(false));
  EXPECT_EQ(Fresh.getAsObject()->getInteger("total"), Code.size());
  EXPECT_EQ(Fresh.getAsObject()->getArray("rows")->size(), 1u);
  auto Timeout = parsed(neverd_comparison_function_page_json(
      C, R"({"address":"0x1000","refresh":true,"timeout_ms":1,"limit":1})"));
  EXPECT_EQ(Timeout.getAsObject()->getString("state"), "timed_out");
  EXPECT_FALSE(
      Timeout.getAsObject()->getBoolean("computation_complete").value_or(true));
  EXPECT_EQ(neverd_comparison_function_page_json(
                C, R"({"address":"0x1000","timeout_ms":-1})"),
            nullptr);
  EXPECT_EQ(neverd_comparison_graph_json(
                C, R"({"address":"0x1000","refresh":"true"})"),
            nullptr);
}

TEST_F(BinaryDiffSDK, AnalystReviewPersistsWithoutBecomingComputedEvidence) {
  auto C = compare(input(body()), input(body(9), 0x8000));
  const auto Before =
      take(neverd_comparison_function_page_json(C, R"({"address":"0x1000"})"));
  const auto Initial = parsed(neverd_comparison_reviews_json(C, "{}"));
  EXPECT_EQ(Initial.getAsObject()->getString("head"), "");
  auto Record =
      llvm::json::Object{{"primary", "0x1000"},
                         {"secondary", "0x8000"},
                         {"case", "owned-case-42"},
                         {"author", "Fixture analyst"},
                         {"note", "Owned test statement; no proof attached."},
                         {"assessment", "candidate_fix"},
                         {"reference", "owned-test:42"},
                         {"expected_head", ""}};
  const auto RecordText = render(llvm::json::Value(llvm::json::Object(Record)));
  ASSERT_EQ(neverd_comparison_review_record_json(C, RecordText.c_str()), 1)
      << take(neverd_comparison_last_error(C));
  const auto First =
      parsed(neverd_comparison_reviews_json(C, R"({"address":"0x1000"})"));
  EXPECT_EQ(First.getAsObject()->getInteger("record_count"), 1);
  EXPECT_EQ(First.getAsObject()->getString("assessment_source"), "analyst");
  const auto Head = First.getAsObject()->getString("head")->str();
  EXPECT_EQ(neverd_comparison_review_record_json(C, RecordText.c_str()), 0);
  Record["expected_head"] = Head;
  Record["assessment"] = "patch_present";
  const auto NextText = render(llvm::json::Value(std::move(Record)));
  ASSERT_EQ(neverd_comparison_review_record_json(C, NextText.c_str()), 1);
  const auto History = take(neverd_comparison_reviews_json(C, "{}"));
  auto Latest = parsed(neverd_comparison_reviews_json(
      C, R"({"latest":true,"case":"owned-case-42"})"));
  EXPECT_EQ(Latest.getAsObject()->getInteger("total"), 1);
  const auto &Last = Latest.getAsObject()->getArray("rows")->front();
  EXPECT_EQ(Last.getAsObject()->getString("supersedes"), Head);
  EXPECT_EQ(Last.getAsObject()->getString("semantic_validation"),
            "not_performed");
  EXPECT_EQ(
      take(neverd_comparison_function_page_json(C, R"({"address":"0x1000"})")),
      Before);
  const auto Saved = take(neverd_comparison_export_json(C));
  ASSERT_EQ(
      neverd_comparison_override_json(
          C, R"({"kind":"unmatch","primary":"0x1000","secondary":"0x8000"})"),
      1);
  const auto Unmatched =
      parsed(neverd_comparison_reviews_json(C, R"({"address":"0x1000"})"));
  EXPECT_FALSE(Unmatched.getAsObject()
                   ->getObject("selection")
                   ->getBoolean("pair_active")
                   .value_or(true));
  for (const auto &Row : *Unmatched.getAsObject()->getArray("rows"))
    EXPECT_FALSE(Row.getAsObject()->getBoolean("pair_active").value_or(true));
  ASSERT_EQ(neverd_comparison_undo(C), 1);
  EXPECT_EQ(take(neverd_comparison_reviews_json(C, "{}")), History);
  ASSERT_EQ(neverd_comparison_restore_json(C, Saved.c_str()), 1);
  EXPECT_EQ(take(neverd_comparison_reviews_json(C, "{}")), History);
  auto Forged = llvm::json::parse(Saved);
  ASSERT_TRUE(bool(Forged));
  auto *Events =
      Forged->getAsObject()->getObject("reviews")->getArray("records");
  (*Events)[0].getAsObject()->operator[]("assessment") = "patch_absent";
  const auto Tampered = render(*Forged);
  EXPECT_EQ(neverd_comparison_restore_json(C, Tampered.c_str()), 0);
  EXPECT_EQ(take(neverd_comparison_reviews_json(C, "{}")), History);
}

TEST_F(BinaryDiffSDK, ChangeObservationStopRefreshAndDeadlineShareOneJob) {
  std::vector<uint8_t> Code(60000, 0x90);
  Code.push_back(0xc3);
  auto C = compare(input(Code), input(Code));
  // Reuse an admitted native delta so cancellation reaches observation
  // indexing.
  ASSERT_FALSE(take(neverd_comparison_function_page_json(
                        C, R"({"address":"0x1000","limit":1})"))
                   .empty());
  auto Future = std::async(std::launch::async, [&] {
    return take(
        neverd_comparison_observations_json(C, R"({"address":"0x1000"})"));
  });
  bool Requested = false;
  const auto Deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (Future.wait_for(std::chrono::milliseconds(0)) !=
             std::future_status::ready &&
         std::chrono::steady_clock::now() < Deadline) {
    const auto Status = parsed(neverd_comparison_status_json(C));
    if (Status.getAsObject()->getString("phase") == "changes:observations" &&
        Status.getAsObject()->getBoolean("running").value_or(false)) {
      neverd_comparison_cancel(C);
      Requested = true;
    }
    std::this_thread::yield();
  }
  ASSERT_EQ(Future.wait_for(std::chrono::seconds(10)),
            std::future_status::ready);
  ASSERT_TRUE(Requested);
  const auto Text = Future.get();
  auto Stopped = llvm::json::parse(Text);
  ASSERT_TRUE(bool(Stopped));
  EXPECT_EQ(Stopped->getAsObject()->getString("state"), "cancelled");
  EXPECT_EQ(
      take(neverd_comparison_observations_json(C, R"({"address":"0x1000"})")),
      Text);
  const auto Expired = parsed(neverd_comparison_observations_json(
      C, R"({"address":"0x1000","refresh":true,"timeout_ms":1})"));
  EXPECT_EQ(Expired.getAsObject()->getString("state"), "timed_out");
  const auto Fresh = parsed(neverd_comparison_observations_json(
      C, R"({"address":"0x1000","refresh":true})"));
  EXPECT_EQ(Fresh.getAsObject()->getString("state"), "complete");
  EXPECT_EQ(Fresh.getAsObject()->getInteger("visited_native_rows"),
            Code.size());
  EXPECT_TRUE(Fresh.getAsObject()->getArray("rows")->empty());
}

TEST_F(BinaryDiffSDK, ChangeObservationsKeepNativePredicateEvidenceAndScope) {
  std::vector<uint8_t> Code{0x83, 0xff, 0x11, 0x74, 0x06, 0xb8,
                            0x02, 0x00, 0x00, 0x00, 0xc3, 0xb8,
                            0x03, 0x00, 0x00, 0x00, 0xc3};
  const auto A = input(Code);
  Code[2] = 9;
  const auto B = input(Code, 0x8000);
  const auto C = compare(A, B, R"({"discover":false,"dataflow":true})");
  const auto Saved = take(neverd_comparison_export_json(C));
  neverd_session_destroy(A);
  neverd_session_destroy(B);
  Sessions[0] = Sessions[1] = nullptr;
  const auto Text = take(neverd_comparison_observations_json(
      C, R"({"address":"0x1000","limit":2})"));
  auto Value = llvm::json::parse(Text);
  ASSERT_TRUE(bool(Value)) << take(neverd_comparison_last_error(C));
  const auto *R = Value->getAsObject();
  ASSERT_NE(R, nullptr);
  EXPECT_EQ(R->getString("state"), "complete");
  EXPECT_EQ(R->getString("security_inference"), "not_performed");
  EXPECT_EQ(R->getString("fix_presence"), "not_assessed");
  EXPECT_EQ(R->getString("semantic_validation"), "not_performed");
  EXPECT_EQ(R->getObject("primary_ssa")->getString("availability"),
            "available");
  ASSERT_NE(R->getArray("rows"), nullptr);
  ASSERT_EQ(R->getArray("rows")->size(), 2u) << Text;
  const auto *Fact = R->getArray("rows")->front().getAsObject();
  ASSERT_NE(Fact, nullptr);
  EXPECT_EQ(Fact->getObject("primary")->getString("bytes"), "7406");
  EXPECT_EQ(Fact->getObject("secondary")->getString("bytes"), "7406");
  EXPECT_EQ(Fact->getString("association"), "direct_synthetic_operand");
  EXPECT_EQ(Fact->getObject("primary_node")->getInteger("origin_sequence"), -1);
  EXPECT_NE(Fact->getInteger("primary_anchor_node"),
            Fact->getObject("primary_node")->getInteger("id"));
  const auto *Native = (*R->getArray("rows"))[1].getAsObject();
  EXPECT_EQ(Native->getObject("primary")->getString("bytes"), "83FF11");
  EXPECT_EQ(Native->getObject("secondary")->getString("bytes"), "83FF09");
  EXPECT_EQ(Native->getString("association"), "native_origin");
  ASSERT_NE(Fact->getObject("primary_node"), nullptr);
  bool Predicate = false, Constant = false;
  for (const auto &Name : *Fact->getArray("primary_context"))
    Predicate |= Name.getAsString() == "predicate_dependency";
  for (const auto &Name : *Fact->getArray("kinds"))
    Constant |= Name.getAsString() == "constant";
  EXPECT_TRUE(Predicate && Constant) << Text;
  EXPECT_EQ(take(neverd_comparison_observations_json(
                C, R"({"side":"secondary","address":"0x8000","limit":2})")),
            Text);
  const auto Filtered = parsed(neverd_comparison_observations_json(
      C, R"({"address":"0x1000","context":"predicate_dependency"})"));
  EXPECT_EQ(Filtered.getAsObject()->getInteger("work"), R->getInteger("work"));
  EXPECT_GT(Filtered.getAsObject()->getInteger("total").value_or(0), 0);
  const auto Partial = parsed(neverd_comparison_observations_json(
      C, R"({"address":"0x1000","max_work":1})"));
  EXPECT_EQ(Partial.getAsObject()->getString("state"), "resource_limited");
  EXPECT_EQ(Partial.getAsObject()->getInteger("visited_native_rows"), 0);
  const auto Confirm =
      R"({"kind":"confirm","primary":"0x1000","secondary":"0x8000"})";
  ASSERT_EQ(neverd_comparison_override_json(C, Confirm), 1);
  const auto Confirmed = parsed(neverd_comparison_observations_json(
      C, R"({"address":"0x1000","limit":1})"));
  EXPECT_TRUE(Confirmed.getAsObject()
                  ->getObject("pairing")
                  ->getBoolean("confirmed")
                  .value_or(false));
  EXPECT_EQ(neverd_comparison_restore_json(C, Saved.c_str()), 1);
  EXPECT_EQ(take(neverd_comparison_observations_json(
                C, R"({"address":"0x1000","limit":2})")),
            Text);
  EXPECT_EQ(neverd_comparison_observations_json(
                C, R"({"address":"0x1000","context":"authorization"})"),
            nullptr);
  EXPECT_EQ(neverd_comparison_observations_json(
                C, R"({"address":"0x1000","max_rows":0})"),
            nullptr);
  EXPECT_EQ(neverd_comparison_observations_json(C, R"({"address":"0x1001"})"),
            nullptr);
}

TEST_F(BinaryDiffSDK,
       CancellationReportsRequestStopAndRetainedPartialSeparately) {
  auto A = input(body()), B = input(body());
  auto C = neverd_comparison_create();
  Comparisons.push_back(C);
  struct Callback {
    neverd_comparison_t C;
    bool ObservedRunning = false;
  } State{C};
  auto Progress = [](void *Context, const char *, neverd_va_t, neverd_va_t) {
    auto &State = *static_cast<Callback *>(Context);
    neverd_comparison_cancel(State.C);
    auto Status = parsed(neverd_comparison_status_json(State.C));
    EXPECT_TRUE(
        Status.getAsObject()->getBoolean("cancel_requested").value_or(false));
    EXPECT_FALSE(
        Status.getAsObject()->getBoolean("computation_stopped").value_or(true));
    State.ObservedRunning = true;
  };
  EXPECT_EQ(
      neverd_comparison_run(C, A, B, R"({"discover":false})", Progress, &State),
      1)
      << take(neverd_comparison_last_error(C));
  EXPECT_TRUE(State.ObservedRunning);
  auto Status = parsed(neverd_comparison_status_json(C));
  EXPECT_EQ(Status.getAsObject()->getString("state"), "cancelled");
  EXPECT_TRUE(
      Status.getAsObject()->getBoolean("computation_stopped").value_or(false));
  EXPECT_TRUE(Status.getAsObject()
                  ->getBoolean("partial_result_retained")
                  .value_or(false));
}

TEST_F(BinaryDiffSDK,
       DiscoveryCancellationKeepsItsPhaseAndIncompleteInventory) {
  auto A = input(body()), B = input(body());
  auto C = neverd_comparison_create();
  Comparisons.push_back(C);
  const auto Cancel = [](void *Context, const char *Phase, neverd_va_t,
                         neverd_va_t) {
    if (std::string_view(Phase) == "primary:discovery")
      neverd_comparison_cancel(Context);
  };
  ASSERT_EQ(neverd_comparison_run(C, A, B, "{}", Cancel, C), 1)
      << take(neverd_comparison_last_error(C));
  const auto Summary = parsed(neverd_comparison_summary_json(C));
  EXPECT_EQ(Summary.getAsObject()->getString("state"), "cancelled");
  EXPECT_EQ(Summary.getAsObject()->getString("stopped_phase"), "discovery");
  const auto Saved = parsed(neverd_comparison_export_json(C));
  ASSERT_TRUE(Saved.getAsObject());
  const auto *Primary = Saved.getAsObject()->getObject("primary");
  const auto *Secondary = Saved.getAsObject()->getObject("secondary");
  ASSERT_TRUE(Primary);
  ASSERT_TRUE(Secondary);
  EXPECT_FALSE(Primary->getBoolean("discovery_complete").value_or(true));
  EXPECT_FALSE(Secondary->getBoolean("discovery_complete").value_or(true));
  // This stopped comparison must not make later discovery believe it is done.
  EXPECT_GE(neverd_session_discover_functions(A), 1);
  EXPECT_GE(neverd_session_discover_functions(B), 1);
}

TEST_F(BinaryDiffSDK, ExhaustedInstructionBudgetCannotLookComplete) {
  auto C = compare(input(body()), input(body()),
                   R"({"discover":false,"max_instructions":1})");
  auto Status = parsed(neverd_comparison_status_json(C));
  EXPECT_EQ(Status.getAsObject()->getString("state"), "resource_limited");
  EXPECT_TRUE(Status.getAsObject()
                  ->getBoolean("partial_result_retained")
                  .value_or(false));
  auto Saved = take(neverd_comparison_export_json(C));
  EXPECT_EQ(neverd_comparison_restore_json(C, Saved.c_str()), 1)
      << take(neverd_comparison_last_error(C));
}

TEST_F(BinaryDiffSDK,
       PagedDetailsAndGraphsShareEvidenceAndInvalidateAfterEdits) {
  auto C = compare(input(body()), input(body(9)));
  auto Full = parsed(neverd_comparison_function_json(C, 0, 0x1000));
  llvm::json::Array Combined;
  for (unsigned Offset = 0; Offset < 3; ++Offset) {
    const auto Query = render(llvm::json::Object{
        {"address", "0x1000"}, {"offset", Offset}, {"limit", 1}});
    auto Page = parsed(neverd_comparison_function_page_json(C, Query.c_str()));
    ASSERT_EQ(Page.getAsObject()->getInteger("total"), 3);
    const auto *Rows = Page.getAsObject()->getArray("rows");
    ASSERT_EQ(Rows->size(), 1u);
    Combined.push_back((*Rows)[0]);
  }
  EXPECT_EQ(Combined, *Full.getAsObject()->getArray("rows"));
  auto Graph =
      parsed(neverd_comparison_graph_json(C, R"({"address":"0x1000"})"));
  ASSERT_EQ(Graph.getAsObject()->getArray("nodes")->size(), 1u);
  const auto *Node = (*Graph.getAsObject()->getArray("nodes"))[0].getAsObject();
  EXPECT_EQ(Node->getObject("comparison")->getString("state"), "changed");
  EXPECT_EQ(Node->getObject("comparison")->getString("counterpart"), "0x1000");
  EXPECT_EQ(
      neverd_comparison_graph_json(C, R"({"address":"0x1000","node_limit":0})"),
      nullptr);
  EXPECT_EQ(neverd_comparison_functions_json(C, R"({"sort":"wrong"})"),
            nullptr);
  ASSERT_EQ(
      neverd_comparison_override_json(
          C, R"({"kind":"unmatch","primary":"0x1000","secondary":"0x1000"})"),
      1);
  auto Unmatched = parsed(neverd_comparison_function_page_json(
      C, R"({"address":"0x1000","limit":1})"));
  EXPECT_TRUE(
      Unmatched.getAsObject()->get("secondary")->getAsNull().has_value());
  EXPECT_EQ(
      (*Unmatched.getAsObject()->getArray("rows"))[0].getAsObject()->getString(
          "kind"),
      "deleted");
  auto Summary = parsed(neverd_comparison_summary_json(C));
  EXPECT_TRUE(Summary.getAsObject()->getBoolean("can_undo").value_or(false));
  EXPECT_EQ(Summary.getAsObject()->get("matches"), nullptr);
}

TEST_F(BinaryDiffSDK, NativeRegionPagesPersistWithoutChangingAcceptedPairs) {
  // mov eax,edi; add eax,3; xor eax,23; imul eax,eax,7; add eax,9; ret.
  const std::vector<uint8_t> Code{0x89, 0xf8, 0x83, 0xc0, 0x03,
                                  0x83, 0xf0, 0x17, 0x6b, 0xc0,
                                  0x07, 0x83, 0xc0, 0x09, 0xc3};
  auto A = input(Code), B = input(Code, 0x8000);
  auto C = compare(A, B, R"({"discover":false,"regions":true})");
  const auto SavedText = take(neverd_comparison_export_json(C));
  auto Page = parsed(neverd_comparison_regions_json(C, R"({"limit":1})"));
  auto *O = Page.getAsObject();
  ASSERT_EQ(O->getInteger("total"), 1);
  const auto *H = (*O->getArray("items"))[0].getAsObject();
  ASSERT_TRUE(H->getString("id"));
  const auto Id = H->getString("id")->str();
  EXPECT_EQ(H->getString("semantic_validation"), "not_performed");
  auto AnchorsQuery =
      render(llvm::json::Object{{"region_id", Id}, {"limit", 1}});
  auto Anchors =
      parsed(neverd_comparison_regions_json(C, AnchorsQuery.c_str()));
  const auto *Anchor =
      (*Anchors.getAsObject()->getArray("items"))[0].getAsObject();
  EXPECT_EQ(Anchor->getObject("primary")->getString("begin"), "0x1000");
  EXPECT_EQ(Anchor->getObject("secondary")->getString("begin"), "0x8000");
  auto Query =
      render(llvm::json::Object{{"region_id", Id},
                                {"anchor", *Anchor->getInteger("anchor")},
                                {"limit", 2}});
  auto Rows = parsed(neverd_comparison_region_page_json(C, Query.c_str()));
  EXPECT_EQ(Rows.getAsObject()->getInteger("total"), 5);
  ASSERT_EQ(Rows.getAsObject()->getArray("rows")->size(), 2u);
  EXPECT_EQ((*Rows.getAsObject()->getArray("rows"))[0]
                .getAsObject()
                ->getObject("primary")
                ->getString("bytes"),
            "89F8");
  EXPECT_EQ(take(neverd_comparison_export_json(C)), SavedText);
  ASSERT_EQ(
      neverd_comparison_override_json(
          C, R"({"kind":"unmatch","primary":"0x1000","secondary":"0x8000"})"),
      1);
  // Region evidence is independent of the editable function bijection.
  auto After = parsed(neverd_comparison_region_page_json(C, Query.c_str()));
  EXPECT_EQ(render(After), render(Rows));
  // Unknown IDs must fail, never selecting another hypothesis by position.
  EXPECT_EQ(neverd_comparison_regions_json(C, R"({"region_id":"missing"})"),
            nullptr);
  EXPECT_EQ(neverd_comparison_restore_json(C, SavedText.c_str()), 1);
  EXPECT_EQ(take(neverd_comparison_export_json(C)), SavedText);
  auto Forged = llvm::json::parse(SavedText);
  ASSERT_TRUE(static_cast<bool>(Forged)) << llvm::toString(Forged.takeError());
  auto *Span =
      (*Forged->getAsObject()->getObject("regions")->getArray("anchors"))[0]
          .getAsObject()
          ->getArray("primary");
  (*Span)[3] = 1000000;
  EXPECT_EQ(neverd_comparison_restore_json(C, render(*Forged).c_str()), 0);
  EXPECT_EQ(take(neverd_comparison_export_json(C)), SavedText);
  auto Without = compare(A, B);
  EXPECT_EQ(neverd_comparison_regions_json(Without, "{}"), nullptr);
}

TEST_F(BinaryDiffSDK, UngroupedRegionEvidenceRemainsPageableAfterBudgetStop) {
  const std::vector<uint8_t> Code{0x89, 0xf8, 0x83, 0xc0, 3,    0x83, 0xf0, 23,
                                  0x6b, 0xc0, 7,    0x83, 0xc0, 9,    0xeb, 0,
                                  0x89, 0xf2, 0x83, 0xc2, 19,   0x83, 0xf2, 93,
                                  0x6b, 0xd2, 11,   0x83, 0xc2, 27,   0xc3};
  auto C = compare(input(Code), input(Code, 0x8000),
                   R"({"discover":false,"regions":{"max_anchors":1}})");
  auto Summary = parsed(neverd_comparison_summary_json(C));
  EXPECT_EQ(Summary.getAsObject()->getString("state"), "resource_limited");
  EXPECT_EQ(Summary.getAsObject()->getObject("regions")->getInteger(
                "hypothesis_count"),
            0);
  auto Page = parsed(
      neverd_comparison_regions_json(C, R"({"all_anchors":true,"limit":1})"));
  EXPECT_EQ(Page.getAsObject()->getInteger("total"), 1);
  EXPECT_TRUE(Page.getAsObject()->get("hypothesis")->getAsNull().has_value());
  auto Rows = parsed(
      neverd_comparison_region_page_json(C, R"({"anchor":0,"limit":2})"));
  EXPECT_EQ(Rows.getAsObject()->getInteger("total"), 5);
  EXPECT_EQ(Rows.getAsObject()->getArray("rows")->size(), 2u);
  EXPECT_TRUE(Rows.getAsObject()->get("hypothesis")->getAsNull().has_value());
  EXPECT_EQ(neverd_comparison_region_page_json(C, R"({"anchor":1})"), nullptr);
  const auto Saved = take(neverd_comparison_export_json(C));
  EXPECT_EQ(neverd_comparison_restore_json(C, Saved.c_str()), 1);
  EXPECT_EQ(take(neverd_comparison_export_json(C)), Saved);
}

TEST_F(BinaryDiffSDK, NativeDataFlowPagesSlicesAndSavedFactsOutliveSessions) {
  const std::vector<uint8_t> Code{0x89, 0xf8, 0x83, 0xc0, 3,    0x83, 0xf0, 23,
                                  0x6b, 0xc0, 7,    0x83, 0xc0, 9,    0xc3};
  auto A = input(Code), B = input(Code, 0x8000);
  auto C = compare(A, B, R"({"discover":false,"dataflow":true})");
  auto Summary = parsed(neverd_comparison_summary_json(C));
  const auto *Flow = Summary.getAsObject()->getObject("dataflow");
  ASSERT_NE(Flow, nullptr);
  EXPECT_EQ(Flow->getObject("primary")->getInteger("available"), 1);
  EXPECT_EQ(Flow->getObject("secondary")->getInteger("available"), 1);
  auto Inventory = parsed(neverd_comparison_dataflow_json(C, "{}"));
  EXPECT_EQ(Inventory.getAsObject()->getInteger("total"), 1);
  auto Page = parsed(neverd_comparison_dataflow_json(
      C, R"({"kind":"nodes","address":"0x1000","limit":200})"));
  const auto *Rows = Page.getAsObject()->getArray("rows");
  ASSERT_NE(Rows, nullptr);
  ASSERT_FALSE(Rows->empty()) << render(Page);
  int64_t Mul = -1;
  for (const auto &Row : *Rows)
    if (Row.getAsObject()->getString("opcode") == "INT_MULT")
      Mul = Row.getAsObject()->getInteger("id").value_or(-1);
  ASSERT_GE(Mul, 0);
  const auto Saved = take(neverd_comparison_export_json(C));
  auto Reopened = compare(A, B, R"({"discover":false,"dataflow":true})");
  EXPECT_EQ(neverd_comparison_restore_json(Reopened, Saved.c_str()), 1)
      << take(neverd_comparison_last_error(Reopened));
  EXPECT_EQ(take(neverd_comparison_export_json(Reopened)), Saved);
  EXPECT_EQ(neverd_comparison_validate_inputs(C, A, B), 1);
  auto Forged = llvm::json::parse(Saved);
  ASSERT_TRUE(static_cast<bool>(Forged)) << llvm::toString(Forged.takeError());
  auto *SavedGraph = Forged->getAsObject()
                         ->getObject("dataflow")
                         ->getObject("primary")
                         ->getArray("entries")
                         ->front()
                         .getAsObject()
                         ->getObject("graph");
  (*SavedGraph->getArray("nodes"))[Mul].getAsObject()->operator[]("opcode") =
      "INT_ADD";
  EXPECT_EQ(neverd_comparison_restore_json(C, render(*Forged).c_str()), 0);
  EXPECT_EQ(take(neverd_comparison_export_json(C)), Saved);
  auto Without = compare(A, B);
  EXPECT_EQ(neverd_comparison_restore_json(Without, Saved.c_str()), 0);
  EXPECT_EQ(neverd_comparison_dataflow_json(Without, "{}"), nullptr);
  neverd_session_destroy(A);
  neverd_session_destroy(B);
  Sessions[0] = Sessions[1] = nullptr;
  llvm::json::Object Query{
      {"address", "0x1000"}, {"seeds", llvm::json::Array{Mul}}, {"limit", 2}};
  auto Slice = parsed(neverd_comparison_slice_json(
      C, render(llvm::json::Object(Query)).c_str()));
  const auto *SliceObject = Slice.getAsObject();
  EXPECT_EQ(SliceObject->getString("state"), "complete");
  ASSERT_EQ(SliceObject->getArray("rows")->size(), 2u);
  EXPECT_GT(SliceObject->getInteger("total").value_or(0), 2);
  Query["offset"] = 2;
  auto Next = parsed(neverd_comparison_slice_json(
      C, render(llvm::json::Object(Query)).c_str()));
  EXPECT_EQ(Next.getAsObject()->getInteger("total"),
            SliceObject->getInteger("total"));
  EXPECT_NE(render(*Next.getAsObject()->get("rows")),
            render(*SliceObject->get("rows")));
  EXPECT_EQ(parsed(neverd_comparison_status_json(C))
                .getAsObject()
                ->getString("phase"),
            "dataflow:slice");
  EXPECT_EQ(neverd_comparison_slice_json(
                C, R"({"address":"0x1000","seeds":[99999]})"),
            nullptr);
  EXPECT_EQ(neverd_comparison_dataflow_json(
                C, R"({"kind":"nodes","address":"0x1000","limit":0})"),
            nullptr);
}

TEST_F(BinaryDiffSDK, DataFlowLimitsRetainNativeMatchesAndUnavailableReasons) {
  auto A = input(body()), B = input(body(8));
  auto C = compare(
      A, B, R"({"discover":false,"dataflow":{"max_native_instructions":1}})");
  auto Summary = parsed(neverd_comparison_summary_json(C));
  EXPECT_EQ(Summary.getAsObject()->getString("state"), "resource_limited");
  EXPECT_EQ(Summary.getAsObject()->getObject("counts")->getInteger("changed"),
            1);
  auto Page = parsed(neverd_comparison_dataflow_json(
      C, R"({"kind":"nodes","address":"0x1000"})"));
  EXPECT_EQ(Page.getAsObject()->getString("availability"), "resource_limited");
  EXPECT_FALSE(Page.getAsObject()->getString("reason")->empty());
  EXPECT_TRUE(Page.getAsObject()->getArray("rows")->empty());
  EXPECT_EQ(
      neverd_comparison_slice_json(C, R"({"address":"0x1000","seeds":[0]})"),
      nullptr);
  const auto Saved = take(neverd_comparison_export_json(C));
  EXPECT_EQ(neverd_comparison_restore_json(C, Saved.c_str()), 1);
  auto Small =
      compare(A, B, R"({"discover":false,"dataflow":{"max_total_nodes":1}})");
  auto SmallSummary = parsed(neverd_comparison_summary_json(Small));
  EXPECT_EQ(SmallSummary.getAsObject()
                ->getObject("dataflow")
                ->getObject("primary")
                ->getInteger("available"),
            0);
}

TEST_F(BinaryDiffSDK,
       SSARegionPagesKeepVerifiedNodesAndOriginalBytesAfterRestore) {
  const std::vector<uint8_t> Code{0x89, 0xf8, 0x83, 0xc0, 3,    0x83, 0xf0, 23,
                                  0x6b, 0xc0, 7,    0x83, 0xc0, 9,    0xc3};
  auto A = input(Code), B = input(Code, 0x8000);
  auto C = compare(A, B, R"({"discover":false,"flow_regions":true})");
  auto Page = parsed(neverd_comparison_flow_regions_json(C, R"({"limit":1})"));
  ASSERT_GT(Page.getAsObject()->getInteger("total").value_or(0), 0);
  const auto *H = Page.getAsObject()->getArray("items")->front().getAsObject();
  const auto Id = H->getString("id")->str();
  EXPECT_TRUE(H->getBoolean("open_control_frontier").value_or(false));
  EXPECT_EQ(H->get("anchors"), nullptr);
  llvm::json::Object Query{{"region_id", Id}, {"limit", 1}};
  auto Anchors = parsed(neverd_comparison_flow_regions_json(
      C, render(llvm::json::Object(Query)).c_str()));
  const auto *Anchor =
      Anchors.getAsObject()->getArray("items")->front().getAsObject();
  EXPECT_EQ(Anchor->get("nodes"), nullptr);
  Query["anchor"] = *Anchor->getInteger("anchor");
  auto Nodes = parsed(neverd_comparison_flow_regions_json(
      C, render(llvm::json::Object(Query)).c_str()));
  ASSERT_EQ(Nodes.getAsObject()->getArray("rows")->size(), 1u);
  const auto *Row =
      Nodes.getAsObject()->getArray("rows")->front().getAsObject();
  for (const char *Side : {"primary", "secondary"}) {
    const auto *N = Row->getObject(Side);
    ASSERT_NE(N, nullptr);
    EXPECT_EQ(N->getObject("original") != nullptr,
              N->getInteger("origin_sequence").value_or(-1) >= 0);
  }
  auto WholeQuery = Query;
  WholeQuery["limit"] = 200;
  auto Whole = parsed(neverd_comparison_flow_regions_json(
      C, render(std::move(WholeQuery)).c_str()));
  unsigned NativePairs = 0;
  for (const auto &Value : *Whole.getAsObject()->getArray("rows")) {
    const auto *Pair = Value.getAsObject();
    const auto *P = Pair->getObject("primary"),
               *S = Pair->getObject("secondary");
    if (const auto *Original = P->getObject("original")) {
      ASSERT_NE(S->getObject("original"), nullptr);
      ASSERT_TRUE(Original->getString("bytes").has_value());
      EXPECT_FALSE(Original->getString("bytes")->empty());
      EXPECT_NE(P->getString("address"), S->getString("address"));
      ++NativePairs;
    }
  }
  EXPECT_GE(NativePairs, 2u);
  auto Saved = take(neverd_comparison_export_json(C));
  auto Reopened = compare(A, B, R"({"discover":false,"flow_regions":true})");
  EXPECT_EQ(neverd_comparison_restore_json(Reopened, Saved.c_str()), 1)
      << take(neverd_comparison_last_error(Reopened));
  EXPECT_EQ(render(parsed(neverd_comparison_flow_regions_json(
                Reopened, render(llvm::json::Object(Query)).c_str()))),
            render(Nodes));
  auto Forged = llvm::json::parse(Saved);
  ASSERT_TRUE(static_cast<bool>(Forged)) << llvm::toString(Forged.takeError());
  (*Forged->getAsObject()->getObject("flow_regions")->getArray("anchors"))[0]
      .getAsObject()
      ->operator[]("primary_root") = 99999;
  EXPECT_EQ(neverd_comparison_restore_json(C, render(*Forged).c_str()), 0);
  EXPECT_EQ(take(neverd_comparison_export_json(C)), Saved);
  neverd_session_destroy(A);
  neverd_session_destroy(B);
  Sessions[0] = Sessions[1] = nullptr;
  Query["offset"] = 1;
  auto Next = parsed(neverd_comparison_flow_regions_json(
      C, render(llvm::json::Object(Query)).c_str()));
  EXPECT_EQ(Next.getAsObject()->getInteger("total"),
            Nodes.getAsObject()->getInteger("total"));
  EXPECT_NE(render(*Next.getAsObject()->get("rows")),
            render(*Nodes.getAsObject()->get("rows")));
  EXPECT_EQ(neverd_comparison_flow_regions_json(C, R"({"anchor":99999})"),
            nullptr);
  EXPECT_EQ(neverd_comparison_flow_regions_json(
                C, R"({"anchor":0,"all_anchors":true})"),
            nullptr);
  EXPECT_EQ(
      neverd_comparison_flow_regions_json(C, R"({"coordinates":"phi_blocks"})"),
      nullptr);
  EXPECT_EQ(neverd_comparison_flow_regions_json(C, R"({"limit":0})"), nullptr);
}

} // namespace

TEST_F(BinaryDiffSDK, OptimizedSSAOriginsSlicesAndTamperRefusal) {
  std::vector<uint8_t> Code{0x89, 0xf8, 0x83, 0xc0, 3,    0x83, 0xf0, 23,
                            0x6b, 0xc0, 7,    0x83, 0xc0, 9,    0xc3};
  auto A = input(Code);
  Code[4] = 5;
  auto B = input(Code, 0x8000);
  const char *Options =
      R"({"discover":false,"dataflow":{"profile":"llvm-ssa-v1"},"flow_regions":true})";
  auto C = compare(A, B, Options);
  const auto Saved = take(neverd_comparison_export_json(C));
  auto Page = parsed(neverd_comparison_dataflow_json(
      C, R"({"kind":"nodes","address":"0x1000","limit":200})"));
  ASSERT_EQ(Page.getAsObject()->getString("availability"), "available")
      << render(Page);
  EXPECT_EQ(Page.getAsObject()->getString("profile"), "llvm-ssa-v1");
  EXPECT_EQ(Page.getAsObject()->getInteger("schema_version"), 3);
  const auto *Rows = Page.getAsObject()->getArray("rows");
  ASSERT_TRUE(Rows && !Rows->empty());
  int64_t NativeNode = -1;
  for (const auto &Row : *Rows) {
    const auto *N = Row.getAsObject();
    EXPECT_EQ(N->getInteger("origin_sequence"), -1);
    EXPECT_EQ(N->getString("address"), "0x0");
    ASSERT_TRUE(N->getArray("origins"));
    if (!N->getArray("origins")->empty())
      NativeNode = *N->getInteger("id");
  }
  ASSERT_GE(NativeNode, 0);
  auto Slice = parsed(neverd_comparison_slice_json(
      C, render(llvm::json::Object{{"address", "0x1000"},
                                   {"seeds", llvm::json::Array{NativeNode}}})
             .c_str()));
  EXPECT_EQ(Slice.getAsObject()->getString("state"), "complete");
  EXPECT_FALSE(Slice.getAsObject()->getArray("rows")->empty());
  const auto Observations =
      parsed(neverd_comparison_observations_json(C, R"({"address":"0x1000"})"));
  const auto *Status = Observations.getAsObject()->getObject("primary_ssa");
  ASSERT_TRUE(Status);
  EXPECT_EQ(Status->getString("availability"), "unsupported_profile");
  EXPECT_FALSE(Status->getString("reason")->empty());
  EXPECT_EQ(Status->getBoolean("indexed"), false);
  auto Reopened = compare(A, B, Options);
  ASSERT_EQ(neverd_comparison_restore_json(Reopened, Saved.c_str()), 1)
      << take(neverd_comparison_last_error(Reopened));
  EXPECT_EQ(take(neverd_comparison_export_json(Reopened)), Saved);
  for (unsigned Kind = 0; Kind < 4; ++Kind) {
    auto Forged = llvm::json::parse(Saved);
    ASSERT_TRUE(static_cast<bool>(Forged))
        << llvm::toString(Forged.takeError());
    auto *Root = Forged->getAsObject();
    auto *Graph = Root->getObject("dataflow")
                      ->getObject("primary")
                      ->getArray("entries")
                      ->front()
                      .getAsObject()
                      ->getObject("graph");
    auto *Node = (*Graph->getArray("nodes"))[NativeNode].getAsObject();
    if (Kind == 0)
      (*Node)["semantics"] = "forged-typed-operation";
    else if (Kind == 1)
      (*Node->getArray("origins"))[0].getAsObject()->operator[]("address") =
          "0xdeadbeef";
    else if (Kind == 2)
      (*Graph)["profile"] = "med-ssa-v2";
    else
      (*Root->getObject("flow_regions"))["profile"] = "med-neighborhoods-v1";
    EXPECT_EQ(neverd_comparison_restore_json(C, render(*Forged).c_str()), 0);
    EXPECT_EQ(take(neverd_comparison_export_json(C)), Saved);
  }
}

TEST_F(BinaryDiffSDK, OptimizedSSAUnsupportedMemoryAndBudgetsRemainVisible) {
  const auto Memory = input({0x8b, 0x07, 0xc3});
  const auto C = compare(
      Memory, Memory,
      R"({"discover":false,"dataflow":{"profile":"llvm-ssa-v1"},"flow_regions":true})");
  const auto Page = parsed(neverd_comparison_dataflow_json(
      C, R"({"kind":"nodes","address":"0x1000"})"));
  EXPECT_EQ(Page.getAsObject()->getString("availability"), "unsupported");
  EXPECT_FALSE(Page.getAsObject()->getString("reason")->empty());
  const auto Summary = parsed(neverd_comparison_summary_json(C));
  EXPECT_EQ(Summary.getAsObject()->getString("state"), "complete");
  EXPECT_EQ(
      Summary.getAsObject()->getObject("flow_regions")->getString("profile"),
      "llvm-neighborhoods-v1");
  const auto Saved = take(neverd_comparison_export_json(C));
  auto Forged = llvm::json::parse(Saved);
  ASSERT_TRUE(static_cast<bool>(Forged)) << llvm::toString(Forged.takeError());
  (*Forged->getAsObject()->getObject("flow_regions"))["profile"] =
      "med-neighborhoods-v1";
  EXPECT_EQ(neverd_comparison_restore_json(C, render(*Forged).c_str()), 0);
  EXPECT_EQ(take(neverd_comparison_export_json(C)), Saved);
  const auto Pure = input({0x89, 0xf8, 0x83, 0xc0, 3, 0xc3});
  const auto Limited = compare(
      Pure, Pure,
      R"({"discover":false,"dataflow":{"profile":"llvm-ssa-v1","graph":{"max_nodes":1}}})");
  const auto LimitedPage = parsed(neverd_comparison_dataflow_json(
      Limited, R"({"kind":"nodes","address":"0x1000"})"));
  EXPECT_EQ(LimitedPage.getAsObject()->getString("availability"),
            "resource_limited");
  EXPECT_FALSE(LimitedPage.getAsObject()->getString("reason")->empty());
}

TEST_F(BinaryDiffSDK, SharedFragmentsPageUncomparedInputsAndReopenExactly) {
  // 64-bit operations expose the changed add directly in both projections;
  // the 32-bit GUI fixture separately exercises optimized cast elimination.
  std::vector<uint8_t> Code{0x48, 0x89, 0xf8, 0x48, 0x83, 0xc0, 3,
                            0x48, 0x83, 0xf0, 23,   0x48, 0x6b, 0xc0,
                            7,    0x48, 0x83, 0xc0, 9,    0xc3};
  auto A = input(Code);
  Code[6] = 5;
  auto B = input(Code, 0x8000);
  for (bool Optimized : {false, true}) {
    SCOPED_TRACE(Optimized ? "LLVM fragments" : "Med fragments");
    const char *Options =
        Optimized
            ? R"({"discover":false,"dataflow":{"profile":"llvm-ssa-v1"},"flow_regions":{"fragments":true}})"
            : R"({"discover":false,"flow_regions":{"fragments":true}})";
    auto C = compare(A, B, Options);
    auto Anchors = parsed(
        neverd_comparison_flow_regions_json(C, R"({"all_anchors":true})"));
    EXPECT_EQ(
        Anchors.getAsObject()->getObject("analysis")->getString("profile"),
        Optimized ? "llvm-fragments-v1" : "med-fragments-v1");
    const auto *Items = Anchors.getAsObject()->getArray("items");
    ASSERT_TRUE(Items && !Items->empty()) << render(Anchors);
    const auto &Anchor = Items->front();
    ASSERT_GT(Anchor.getAsObject()->getInteger("frontier_count").value_or(0),
              0);
    llvm::json::Object Query{
        {"anchor", *Anchor.getAsObject()->getInteger("anchor")},
        {"coordinates", "frontier"},
        {"limit", 1}};
    const auto Frontier = parsed(neverd_comparison_flow_regions_json(
        C, render(llvm::json::Object(Query)).c_str()));
    EXPECT_EQ(Frontier.getAsObject()->getBoolean("boundary_only"), true);
    EXPECT_EQ(Frontier.getAsObject()->getString("view"), "frontier");
    const auto *Rows = Frontier.getAsObject()->getArray("rows");
    ASSERT_TRUE(Rows && Rows->size() == 1);
    for (const auto &[Side, Value] :
         {std::pair{"primary", "0x3"}, {"secondary", "0x5"}}) {
      const auto *Node = Rows->front().getAsObject()->getObject(Side);
      ASSERT_TRUE(Node);
      EXPECT_EQ(Node->getString("opcode"), Optimized ? "add" : "INT_ADD");
      if (Optimized) {
        ASSERT_TRUE(Node->getArray("originals"));
        EXPECT_FALSE(Node->getArray("originals")->empty());
      } else {
        ASSERT_TRUE(Node->getObject("original"));
        EXPECT_FALSE(Node->getObject("original")->getString("bytes")->empty());
      }
      bool HasConstant = false;
      for (const auto &Input : *Node->getArray("inputs"))
        HasConstant |= (*Input.getAsArray())[0].getAsInteger() == 1 &&
                       (*Input.getAsArray())[3].getAsString() == Value;
      EXPECT_TRUE(HasConstant) << render(llvm::json::Object(*Node));
    }
    const auto Saved = take(neverd_comparison_export_json(C));
    auto Reopened = compare(A, B, Options);
    ASSERT_EQ(neverd_comparison_restore_json(Reopened, Saved.c_str()), 1)
        << take(neverd_comparison_last_error(Reopened));
    EXPECT_EQ(take(neverd_comparison_export_json(Reopened)), Saved);
    EXPECT_EQ(render(parsed(neverd_comparison_flow_regions_json(
                  Reopened, render(llvm::json::Object(Query)).c_str()))),
              render(Frontier));
    auto Forged = llvm::json::parse(Saved);
    ASSERT_TRUE(static_cast<bool>(Forged))
        << llvm::toString(Forged.takeError());
    (*Forged->getAsObject()->getObject("flow_regions")->getArray("anchors"))[0]
        .getAsObject()
        ->operator[]("frontier") = llvm::json::Array{};
    EXPECT_EQ(neverd_comparison_restore_json(C, render(*Forged).c_str()), 0);
    EXPECT_EQ(take(neverd_comparison_export_json(C)), Saved);
    Query["offset"] = *Frontier.getAsObject()->getInteger("total");
    EXPECT_TRUE(parsed(neverd_comparison_flow_regions_json(
                           C, render(llvm::json::Object(Query)).c_str()))
                    .getAsObject()
                    ->getArray("rows")
                    ->empty());
  }
  const auto Legacy = compare(
      A, B, R"({"discover":false,"dataflow":true,"flow_regions":true})");
  EXPECT_EQ(neverd_comparison_flow_regions_json(
                Legacy, R"({"anchor":0,"coordinates":"frontier"})"),
            nullptr);
}

TEST_F(BinaryDiffSDK, OptimizedSSAEarlyCancellationRetainsReopenableProfile) {
  for (bool Fragments : {false, true})
    for (const auto *Phase : {"primary:discovery", "primary:decode"}) {
      auto A = input(body()), B = input(body());
      auto C = neverd_comparison_create();
      Comparisons.push_back(C);
      struct State {
        neverd_comparison_t Comparison;
        std::string Phase;
      } Context{C, Phase};
      const auto Cancel = [](void *Opaque, const char *Phase, neverd_va_t,
                             neverd_va_t) {
        auto &S = *static_cast<State *>(Opaque);
        if (S.Phase == Phase)
          neverd_comparison_cancel(S.Comparison);
      };
      ASSERT_EQ(
          neverd_comparison_run(
              C, A, B,
              Fragments
                  ? R"({"dataflow":{"profile":"llvm-ssa-v1"},"flow_regions":{"fragments":true}})"
                  : R"({"dataflow":{"profile":"llvm-ssa-v1"},"flow_regions":true})",
              Cancel, &Context),
          1)
          << take(neverd_comparison_last_error(C));
      const auto Saved = take(neverd_comparison_export_json(C));
      auto Document = llvm::json::parse(Saved);
      ASSERT_TRUE(bool(Document));
      EXPECT_EQ(Document->getAsObject()->getString("state"), "cancelled");
      EXPECT_EQ(Document->getAsObject()
                    ->getObject("flow_regions")
                    ->getString("profile"),
                Fragments ? "llvm-fragments-v1" : "llvm-neighborhoods-v1");
      EXPECT_EQ(neverd_comparison_restore_json(C, Saved.c_str()), 1)
          << take(neverd_comparison_last_error(C));
      EXPECT_EQ(take(neverd_comparison_export_json(C)), Saved);
    }
}

TEST_F(BinaryDiffSDK, ParameterViewsPageAndBindTheOriginalSourceSignature) {
  // Owned control_regions_fixture.c family 1, x64 ELF O0/O2. Analysis only.
  const auto Raw = [&](llvm::StringRef Hex, uint64_t Base) {
    const auto Bytes = llvm::fromHex(Hex);
    return input(
        llvm::ArrayRef<uint8_t>(reinterpret_cast<const uint8_t *>(Bytes.data()),
                                Bytes.size()),
        Base);
  };
  const auto A =
      Raw("554889e5897dfc8975f88b45fc3b45f873118b45fc83f0176bc00783c009"
          "8945fceb0f8b45fc83c0116b4df80d31c88945fc8b45fc3345f883c0035dc3",
          0x1000);
  const auto B =
      Raw("39f7731183f7178d04fd0000000029f883c009eb0b83c7118d04768d0486"
          "31f831f083c003c3",
          0x8000);
  const auto Options =
      R"({"discover":false,"dataflow":{"profile":"llvm-parameter-ssa-v1"},"flow_regions":true})";
  const auto C = compare(A, B, Options);
  const auto Saved = take(neverd_comparison_export_json(C));
  auto Summary = parsed(neverd_comparison_summary_json(C));
  EXPECT_GT(Summary.getAsObject()
                ->getObject("flow_regions")
                ->getInteger("anchor_count"),
            0);
  auto Page = parsed(neverd_comparison_dataflow_json(
      C,
      R"({"kind":"nodes","side":"secondary","address":"0x8000","limit":200})"));
  const auto *P = Page.getAsObject();
  ASSERT_EQ(P->getString("availability"), "available") << render(Page);
  EXPECT_EQ(P->getInteger("schema_version"), 4);
  EXPECT_EQ(P->getInteger("projected_parameter_count"), 1);
  EXPECT_FALSE(P->get("parameter_views"));
  int64_t ProjectedNode = -1;
  for (const auto &Row : *P->getArray("rows"))
    for (const auto &View : *Row.getAsObject()->getArray("parameter_views"))
      if (View.getAsObject()->getInteger("source_bits") == 64) {
        EXPECT_EQ(View.getAsObject()->getInteger("view_bits"), 32);
        EXPECT_EQ(View.getAsObject()->getInteger("elided_truncations"), 1);
        ProjectedNode = *Row.getAsObject()->getInteger("id");
      }
  ASSERT_GE(ProjectedNode, 0);
  const auto Slice = parsed(neverd_comparison_slice_json(
      C, render(llvm::json::Object{{"side", "secondary"},
                                   {"address", "0x8000"},
                                   {"seeds", llvm::json::Array{ProjectedNode}}})
             .c_str()));
  EXPECT_EQ(Slice.getAsObject()->getInteger("schema_version"), 4);
  const auto Reopened = compare(A, B, Options);
  ASSERT_EQ(neverd_comparison_restore_json(Reopened, Saved.c_str()), 1)
      << take(neverd_comparison_last_error(Reopened));
  EXPECT_EQ(take(neverd_comparison_export_json(Reopened)), Saved);
  for (unsigned Kind = 0; Kind < 3; ++Kind) {
    auto Forged = llvm::json::parse(Saved);
    ASSERT_TRUE(static_cast<bool>(Forged))
        << llvm::toString(Forged.takeError());
    auto *Graph = Forged->getAsObject()
                      ->getObject("dataflow")
                      ->getObject("secondary")
                      ->getArray("entries")
                      ->front()
                      .getAsObject()
                      ->getObject("graph");
    auto *V = (*Graph->getArray("parameter_views"))[1].getAsObject();
    if (Kind == 0) {
      // A structurally valid alternative source; only fresh binding can refute
      // it.
      (*V)["source_bits"] = 32;
      (*V)["elided_truncations"] = 0;
      (*Graph)["projected_parameter_count"] = 0;
    } else if (Kind == 1) {
      (*V->getObject("native_site"))["index"] = "0x999";
    } else {
      (*Graph)["parameter_count"] = 0;
    }
    EXPECT_EQ(neverd_comparison_restore_json(C, render(*Forged).c_str()), 0);
    EXPECT_EQ(take(neverd_comparison_export_json(C)), Saved);
  }
  const auto Observations =
      parsed(neverd_comparison_observations_json(C, R"({"address":"0x1000"})"));
  EXPECT_EQ(Observations.getAsObject()
                ->getObject("primary_ssa")
                ->getString("availability"),
            "unsupported_profile");
  const auto Limited = compare(
      B, B,
      R"({"discover":false,"dataflow":{"profile":"llvm-parameter-ssa-v1","max_total_nodes":15}})");
  const auto LimitedPage = parsed(neverd_comparison_dataflow_json(
      Limited, R"({"kind":"nodes","address":"0x8000"})"));
  EXPECT_EQ(LimitedPage.getAsObject()->getString("availability"),
            "resource_limited");
}

TEST_F(BinaryDiffSDK,
       ParameterViewsStoppedAndEmptyCollectionsKeepTheirProfile) {
  for (const auto *Profile :
       {"llvm-parameter-ssa-v1", "llvm-progress-ssa-v1", "llvm-progress-ssa-v2",
        "llvm-progress-ssa-v3", "llvm-progress-ssa-v4", "llvm-progress-ssa-v5",
        "llvm-progress-ssa-v6", "llvm-progress-ssa-v7"})
    for (bool Fragments : {false, true})
      for (const auto *Phase :
           {"primary:discovery", "primary:decode", "primary:dataflow"}) {
        const auto A = input(body()), B = input(body());
        const auto C = neverd_comparison_create();
        Comparisons.push_back(C);
        struct State {
          neverd_comparison_t Comparison;
          std::string Phase;
        } Context{C, Phase};
        const auto Cancel = [](void *Opaque, const char *Current, neverd_va_t,
                               neverd_va_t) {
          auto &S = *static_cast<State *>(Opaque);
          if (S.Phase == Current)
            neverd_comparison_cancel(S.Comparison);
        };
        const auto Options = render(llvm::json::Object{
            {"dataflow", llvm::json::Object{{"profile", Profile}}},
            {"flow_regions", llvm::json::Object{{"fragments", Fragments}}}});
        ASSERT_EQ(
            neverd_comparison_run(C, A, B, Options.c_str(), Cancel, &Context),
            1)
            << take(neverd_comparison_last_error(C));
        const auto Saved = take(neverd_comparison_export_json(C));
        auto Document = llvm::json::parse(Saved);
        ASSERT_TRUE(bool(Document));
        EXPECT_EQ(Document->getAsObject()->getString("state"), "cancelled")
            << Phase;
        EXPECT_EQ(Document->getAsObject()
                      ->getObject("flow_regions")
                      ->getString("profile"),
                  std::string(Profile) == "llvm-progress-ssa-v7"
                      ? (Fragments ? "llvm-progress-fragments-v7"
                                   : "llvm-progress-neighborhoods-v7")
                  : std::string(Profile) == "llvm-progress-ssa-v6"
                      ? (Fragments ? "llvm-progress-fragments-v6"
                                   : "llvm-progress-neighborhoods-v6")
                  : std::string(Profile) == "llvm-progress-ssa-v5"
                      ? (Fragments ? "llvm-progress-fragments-v5"
                                   : "llvm-progress-neighborhoods-v5")
                  : std::string(Profile) == "llvm-progress-ssa-v4"
                      ? (Fragments ? "llvm-progress-fragments-v4"
                                   : "llvm-progress-neighborhoods-v4")
                  : std::string(Profile) == "llvm-progress-ssa-v3"
                      ? (Fragments ? "llvm-progress-fragments-v3"
                                   : "llvm-progress-neighborhoods-v3")
                  : std::string(Profile) == "llvm-progress-ssa-v2"
                      ? (Fragments ? "llvm-progress-fragments-v2"
                                   : "llvm-progress-neighborhoods-v2")
                  : std::string(Profile) == "llvm-progress-ssa-v1"
                      ? (Fragments ? "llvm-progress-fragments-v1"
                                   : "llvm-progress-neighborhoods-v1")
                      : (Fragments ? "llvm-parameter-fragments-v1"
                                   : "llvm-parameter-neighborhoods-v1"));
        EXPECT_EQ(neverd_comparison_restore_json(C, Saved.c_str()), 1)
            << take(neverd_comparison_last_error(C));
        EXPECT_EQ(take(neverd_comparison_export_json(C)), Saved);
        (*Document->getAsObject()->getObject("flow_regions"))["profile"] =
            Fragments ? "llvm-fragments-v1" : "llvm-neighborhoods-v1";
        EXPECT_EQ(neverd_comparison_restore_json(C, render(*Document).c_str()),
                  0);
        EXPECT_EQ(take(neverd_comparison_export_json(C)), Saved);
      }
}

TEST_F(BinaryDiffSDK, LoopProgressPagesBindWitnessesAndOriginalContracts) {
  // Owned control_regions_fixture.c family 2, x64 ELF O0/O2. Analysis only.
  const auto Raw = [&](llvm::StringRef Hex, uint64_t Base) {
    const auto Bytes = llvm::fromHex(Hex);
    return input(
        llvm::ArrayRef<uint8_t>(reinterpret_cast<const uint8_t *>(Bytes.data()),
                                Bytes.size()),
        Base);
  };
  const auto A = Raw(
      "554889e5897dfc8975f88b45f883e0078945f48b45f489c183c1ff894df483f80074228b"
      "45fc83f0176bc00783c0098945fc8b45fcc1e8058b4dfcc1e11b09c88945fcebce8b45fc"
      "3345f883c0035dc3",
      0x1000);
  const auto B = Raw(
      "89f889f183e1070f84ae00000083f0178d14c50000000029c283c20989d0c1c01b83f901"
      "0f849100000083f0178d14c50000000029c283c20989d0c1c01b83f902747883f0178d14"
      "c50000000029c283c20989d0c1c01b83f903745f83f0178d14c50000000029c283c20989"
      "d0c1c01b83f904744683f0178d14c50000000029c283c20989d0c1c01b83f905742d83f0"
      "178d14c50000000029c283c20989d0c1c01b83f906741483f0178d0cc50000000029c183"
      "c109c1c11b89c831f083c003c3",
      0x8000);
  const auto Options =
      R"({"discover":false,"dataflow":{"profile":"llvm-progress-ssa-v1"},"flow_regions":{"fragments":true}})";
  const auto C = compare(A, B, Options);
  const auto Saved = take(neverd_comparison_export_json(C));
  auto Summary = parsed(neverd_comparison_summary_json(C));
  EXPECT_GT(Summary.getAsObject()
                ->getObject("flow_regions")
                ->getInteger("anchor_count"),
            0);
  EXPECT_EQ(
      Summary.getAsObject()->getObject("flow_regions")->getString("profile"),
      "llvm-progress-fragments-v1");
  for (const auto *Side : {"primary", "secondary"}) {
    const bool Primary = std::string(Side) == "primary";
    auto Page = parsed(neverd_comparison_dataflow_json(
        C, render(llvm::json::Object{{"kind", "nodes"},
                                     {"side", Side},
                                     {"address", Primary ? "0x1000" : "0x8000"},
                                     {"limit", 200}})
               .c_str()));
    const auto *P = Page.getAsObject();
    ASSERT_EQ(P->getString("availability"), "available") << render(Page);
    EXPECT_EQ(P->getInteger("schema_version"), 5);
    const auto *Proof = P->getObject("progress");
    ASSERT_TRUE(Proof);
    EXPECT_EQ(Proof->getString("kind"), Primary ? "counter_loops" : "acyclic");
    EXPECT_EQ(Proof->getInteger("loop_count"), Primary ? 1 : 0);
    EXPECT_FALSE(Proof->get("loops"));
    if (Primary) {
      bool Seen = false;
      for (const auto &Row : *P->getArray("rows"))
        if (const auto *Loop = Row.getAsObject()->getObject("progress_loop")) {
          Seen = true;
          const auto Slice = parsed(neverd_comparison_slice_json(
              C, render(llvm::json::Object{
                            {"side", Side},
                            {"address", "0x1000"},
                            {"seeds",
                             llvm::json::Array{*Loop->getInteger("update")}}})
                     .c_str()));
          EXPECT_EQ(Slice.getAsObject()->getInteger("schema_version"), 5);
          break;
        }
      EXPECT_TRUE(Seen);
    }
  }
  const auto Reopened = compare(A, B, Options);
  ASSERT_EQ(neverd_comparison_restore_json(Reopened, Saved.c_str()), 1)
      << take(neverd_comparison_last_error(Reopened));
  EXPECT_EQ(take(neverd_comparison_export_json(Reopened)), Saved);
  auto Forged = llvm::json::parse(Saved);
  ASSERT_TRUE(static_cast<bool>(Forged)) << llvm::toString(Forged.takeError());
  auto *Graph = Forged->getAsObject()
                    ->getObject("dataflow")
                    ->getObject("primary")
                    ->getArray("entries")
                    ->front()
                    .getAsObject()
                    ->getObject("graph");
  auto *Proof = Graph->getObject("progress");
  // Structurally plausible alternate source attributes require fresh binding.
  (*Proof)["source_contract"] = Proof->getString("observed_contract")->str();
  EXPECT_EQ(neverd_comparison_restore_json(C, render(*Forged).c_str()), 0);
  EXPECT_NE(take(neverd_comparison_last_error(C)).find("native SSA"),
            std::string::npos);
  EXPECT_EQ(take(neverd_comparison_export_json(C)), Saved);
  const auto Limited = compare(
      A, A,
      R"({"discover":false,"dataflow":{"profile":"llvm-progress-ssa-v1","max_total_nodes":19}})");
  const auto LimitedPage = parsed(neverd_comparison_dataflow_json(
      Limited, R"({"kind":"nodes","address":"0x1000"})"));
  EXPECT_EQ(LimitedPage.getAsObject()->getString("availability"),
            "resource_limited");
}

TEST_F(BinaryDiffSDK, MultiBlockProgressBindsPagedHeadersAndFreshSource) {
  // Owned control_regions_fixture.c family3, clang x64 ELF O0. Inspect only.
  const auto Bytes = llvm::fromHex(
      "554889e5897dfc8975f88b45f883e0078945f48b45f489c183c1ff894df483f800746e8b"
      "45fc83e00183f80074528b45fc83f0176bc00783c0098945fc8b45fcc1e8058b4dfcc1e1"
      "1b09c88945fc8b45fc05674523013345f88945fc8b45fc8b4dfcc1e90b31c86bc00d8945"
      "fc8b45fcc1e0078b4dfcc1e91909c88945fceb0f8b45fc83c0116b4df80d31c88945fceb"
      "828b45fc3345f883c0035dc3");
  const auto A =
      input(llvm::ArrayRef<uint8_t>(
                reinterpret_cast<const uint8_t *>(Bytes.data()), Bytes.size()),
            0x1000);
  const auto B =
      input(llvm::ArrayRef<uint8_t>(
                reinterpret_cast<const uint8_t *>(Bytes.data()), Bytes.size()),
            0x8000);
  const auto Options =
      R"({"discover":false,"dataflow":{"profile":"llvm-progress-ssa-v2"},"flow_regions":{"fragments":true}})";
  const auto C = compare(A, B, Options);
  const auto Saved = take(neverd_comparison_export_json(C));
  const auto Summary = parsed(neverd_comparison_summary_json(C));
  EXPECT_EQ(
      Summary.getAsObject()->getObject("flow_regions")->getString("profile"),
      "llvm-progress-fragments-v2");
  auto Page = parsed(neverd_comparison_dataflow_json(
      C, R"({"kind":"nodes","address":"0x1000","limit":200})"));
  ASSERT_EQ(Page.getAsObject()->getString("availability"), "available")
      << render(Page);
  EXPECT_EQ(Page.getAsObject()->getInteger("schema_version"), 6);
  ASSERT_EQ(Page.getAsObject()->getObject("progress")->getString("kind"),
            "counter_loops")
      << render(Page);
  EXPECT_FALSE(Page.getAsObject()->getObject("progress")->get("loops"));
  bool Seen = false;
  for (const auto &Row : *Page.getAsObject()->getArray("rows"))
    if (const auto *Loop = Row.getAsObject()->getObject("progress_loop")) {
      Seen = true;
      EXPECT_NE(Loop->getInteger("header"), Loop->getInteger("block"));
      auto Slice = parsed(neverd_comparison_slice_json(
          C,
          render(llvm::json::Object{
                     {"address", "0x1000"},
                     {"seeds", llvm::json::Array{*Loop->getInteger("update")}}})
              .c_str()));
      EXPECT_EQ(Slice.getAsObject()->getInteger("schema_version"), 6);
      break;
    }
  EXPECT_TRUE(Seen);
  const auto Reopened = compare(A, B, Options);
  ASSERT_EQ(neverd_comparison_restore_json(Reopened, Saved.c_str()), 1)
      << take(neverd_comparison_last_error(Reopened));
  EXPECT_EQ(take(neverd_comparison_export_json(Reopened)), Saved);
  for (const auto Header : {false, true}) {
    auto V = llvm::json::parse(Saved);
    ASSERT_TRUE(static_cast<bool>(V)) << llvm::toString(V.takeError());
    auto *Proof = V->getAsObject()
                      ->getObject("dataflow")
                      ->getObject("primary")
                      ->getArray("entries")
                      ->front()
                      .getAsObject()
                      ->getObject("graph")
                      ->getObject("progress");
    if (Header) {
      auto *L = Proof->getArray("loops")->front().getAsObject();
      (*L)["header"] = *L->getInteger("block");
    } else
      (*Proof)["source_contract"] =
          Proof->getString("observed_contract")->str();
    EXPECT_EQ(neverd_comparison_restore_json(C, render(*V).c_str()), 0);
    EXPECT_EQ(take(neverd_comparison_export_json(C)), Saved);
  }
}

TEST_F(BinaryDiffSDK, NestedProgressPagesEveryLayerAndRechecksFreshSource) {
  // Owned control_regions_fixture.c family4, clang x64 ELF O0. Inspect only.
  const auto Bytes = llvm::fromHex(
      "554889e5897dfc8975f88b45f883e0078945f48b45f489c183c1ff894df483f800745b"
      "8b45fc83e00783c0018945f08b45f089c183c1ff894df083f800742e8b45fc83f0176b"
      "c00783c0098945fc8b45fcc1e8058b4dfcc1e11b09c88945fc8b45fc0345f43345f889"
      "45fcebc28b45fcc1e0070345f43345fc8945fceb958b45fc3345f883c0035dc3");
  const auto Raw = llvm::ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(Bytes.data()), Bytes.size());
  const auto A = input(Raw, 0x1000), B = input(Raw, 0x8000);
  const auto Options =
      R"({"discover":false,"dataflow":{"profile":"llvm-progress-ssa-v3"},"flow_regions":{"fragments":true}})";
  const auto C = compare(A, B, Options);
  const auto Summary = parsed(neverd_comparison_summary_json(C));
  EXPECT_EQ(
      Summary.getAsObject()->getObject("flow_regions")->getString("profile"),
      "llvm-progress-fragments-v3");
  std::set<int64_t> Latches;
  uint64_t Total = 1;
  for (uint64_t Offset = 0; Offset < Total; ++Offset) {
    auto Page = parsed(neverd_comparison_dataflow_json(
        C, render(llvm::json::Object{{"kind", "nodes"},
                                     {"address", "0x1000"},
                                     {"limit", 1},
                                     {"offset", Offset}})
               .c_str()));
    const auto *O = Page.getAsObject();
    ASSERT_EQ(O->getString("availability"), "available") << render(Page);
    EXPECT_EQ(O->getInteger("schema_version"), 7);
    ASSERT_EQ(O->getObject("progress")->getString("kind"), "counter_loops")
        << render(Page);
    EXPECT_EQ(O->getObject("progress")->getInteger("loop_count"), 2);
    EXPECT_FALSE(O->getObject("progress")->get("loops"));
    Total = *O->getInteger("total");
    const auto *Rows = O->getArray("rows");
    ASSERT_EQ(Rows->size(), 1u);
    const auto *Row = Rows->front().getAsObject();
    if (const auto *Loop = Row->getObject("progress_loop")) {
      EXPECT_EQ(Loop->getInteger("block"), Row->getInteger("block"));
      EXPECT_LT(*Loop->getInteger("header"), *O->getInteger("block_count"));
      if (Latches.insert(*Loop->getInteger("block")).second) {
        const auto Slice = parsed(neverd_comparison_slice_json(
            C, render(llvm::json::Object{
                          {"address", "0x1000"},
                          {"seeds",
                           llvm::json::Array{*Loop->getInteger("update")}}})
                   .c_str()));
        EXPECT_EQ(Slice.getAsObject()->getInteger("schema_version"), 7);
      }
    }
  }
  EXPECT_EQ(Latches.size(), 2u);
  const auto Saved = take(neverd_comparison_export_json(C));
  const auto Reopened = compare(A, B, Options);
  ASSERT_EQ(neverd_comparison_restore_json(Reopened, Saved.c_str()), 1)
      << take(neverd_comparison_last_error(Reopened));
  EXPECT_EQ(take(neverd_comparison_export_json(Reopened)), Saved);
  for (unsigned Forgery = 0; Forgery < 3; ++Forgery) {
    auto V = llvm::json::parse(Saved);
    ASSERT_TRUE(static_cast<bool>(V)) << llvm::toString(V.takeError());
    auto *Proof = V->getAsObject()
                      ->getObject("dataflow")
                      ->getObject("primary")
                      ->getArray("entries")
                      ->front()
                      .getAsObject()
                      ->getObject("graph")
                      ->getObject("progress");
    if (Forgery < 2)
      (*(*Proof->getArray("loops"))[Forgery].getAsObject())["header"] = 0;
    else
      (*Proof)["source_contract"] =
          Proof->getString("observed_contract")->str();
    EXPECT_EQ(neverd_comparison_restore_json(C, render(*V).c_str()), 0);
    EXPECT_EQ(take(neverd_comparison_export_json(C)), Saved);
  }
}

void BinaryDiffSDK::checkBranchProgress(llvm::StringRef Hex, unsigned Version,
                                        unsigned GuardCount,
                                        llvm::StringRef ProofKind) {
  const auto Bytes = llvm::fromHex(Hex);
  const auto Raw = llvm::ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(Bytes.data()), Bytes.size());
  const auto A = input(Raw, 0x1000), B = input(Raw, 0x8000);
  const auto Options = render(llvm::json::Object{
      {"discover", false},
      {"dataflow",
       llvm::json::Object{
           {"profile", "llvm-progress-ssa-v" + std::to_string(Version)}}},
      {"flow_regions", llvm::json::Object{{"fragments", true}}}});
  const auto C = compare(A, B, Options.c_str());
  const auto Summary = parsed(neverd_comparison_summary_json(C));
  EXPECT_EQ(
      Summary.getAsObject()->getObject("flow_regions")->getString("profile"),
      "llvm-progress-fragments-v" + std::to_string(Version));
  EXPECT_GT(Summary.getAsObject()
                ->getObject("flow_regions")
                ->getInteger("anchor_count"),
            0);
  std::set<int64_t> Guards, Latches;
  uint64_t Total = 1;
  for (uint64_t Offset = 0; Offset < Total; ++Offset) {
    auto Page = parsed(neverd_comparison_dataflow_json(
        C, render(llvm::json::Object{{"kind", "nodes"},
                                     {"address", "0x1000"},
                                     {"limit", 1},
                                     {"offset", Offset}})
               .c_str()));
    const auto *O = Page.getAsObject();
    ASSERT_EQ(O->getString("availability"), "available") << render(Page);
    EXPECT_EQ(O->getInteger("schema_version"), 4 + Version);
    ASSERT_EQ(O->getObject("progress")->getString("kind"), "counter_loops")
        << render(Page);
    EXPECT_EQ(O->getObject("progress")->getInteger("loop_count"), 1);
    EXPECT_EQ(O->getObject("progress")->getInteger("backedge_count"), 1);
    EXPECT_EQ(O->getObject("progress")->getInteger("guard_count"), GuardCount);
    EXPECT_FALSE(O->getObject("progress")->get("loops"));
    Total = *O->getInteger("total");
    const auto *Rows = O->getArray("rows");
    ASSERT_EQ(Rows->size(), 1u);
    const auto *Row = Rows->front().getAsObject();
    if (const auto *Loop = Row->getObject("progress_loop")) {
      EXPECT_EQ(Loop->getInteger("block"), Row->getInteger("block"));
      EXPECT_LT(*Loop->getInteger("header"), *O->getInteger("block_count"));
      Latches.insert(*Loop->getInteger("latch"));
      if (Version == 4)
        EXPECT_NE(Loop->getInteger("latch"), Loop->getInteger("block"));
      else
        EXPECT_EQ(Loop->getString("proof"), ProofKind);
      if (Guards.insert(*Loop->getInteger("block")).second) {
        const auto Slice = parsed(neverd_comparison_slice_json(
            C, render(llvm::json::Object{
                          {"address", "0x1000"},
                          {"seeds",
                           llvm::json::Array{*Loop->getInteger("update")}}})
                   .c_str()));
        EXPECT_EQ(Slice.getAsObject()->getInteger("schema_version"),
                  4 + Version);
      }
    }
  }
  EXPECT_EQ(Guards.size(), GuardCount);
  EXPECT_EQ(Latches.size(), 1u);
  const auto Saved = take(neverd_comparison_export_json(C));
  const auto Reopened = compare(A, B, Options.c_str());
  ASSERT_EQ(neverd_comparison_restore_json(Reopened, Saved.c_str()), 1)
      << take(neverd_comparison_last_error(Reopened));
  EXPECT_EQ(take(neverd_comparison_export_json(Reopened)), Saved);
  for (unsigned Forgery = 0;
       Forgery < 2 * GuardCount + 4 + (Version >= 5 ? 4 : 0); ++Forgery) {
    auto V = llvm::json::parse(Saved);
    ASSERT_TRUE(static_cast<bool>(V)) << llvm::toString(V.takeError());
    auto *Proof = V->getAsObject()
                      ->getObject("dataflow")
                      ->getObject("primary")
                      ->getArray("entries")
                      ->front()
                      .getAsObject()
                      ->getObject("graph")
                      ->getObject("progress");
    if (Forgery < GuardCount)
      (*(*Proof->getArray("loops"))[Forgery].getAsObject())["header"] = 0;
    else if (Forgery < 2 * GuardCount)
      (*(*Proof->getArray("loops"))[Forgery - GuardCount]
            .getAsObject())["latch"] = 0;
    else if (Forgery < 2 * GuardCount + 3)
      (*Proof)[std::array<const char *, 3>{
          "loop_count", "backedge_count",
          "guard_count"}[Forgery - 2 * GuardCount]] = 99;
    else if (Forgery == 2 * GuardCount + 3)
      (*Proof)["source_contract"] =
          Proof->getString("observed_contract")->str();
    else {
      auto *Loop = Proof->getArray("loops")->front().getAsObject();
      if (Forgery == 2 * GuardCount + 4)
        Loop->erase("proof");
      else
        (*Loop)["proof"] = Forgery == 2 * GuardCount + 5
                               ? (ProofKind == "nowrap" ? "modular" : "nowrap")
                           : Forgery == 2 * GuardCount + 6 ? "none"
                                                           : "unsupported";
    }
    EXPECT_EQ(neverd_comparison_restore_json(C, render(*V).c_str()), 0);
    EXPECT_EQ(take(neverd_comparison_export_json(C)), Saved);
  }
}

TEST_F(BinaryDiffSDK, SelectedUpdatesPagesEveryGuardAndRechecksFreshSource) {
  // Owned branch_progress_fixture.c family5, clang x64 ELF O0. Inspect only.
  checkBranchProgress(
      "554889e5897dfc8975f88b45f883e00783c0018945f48b45fc83e00183f80074328b45"
      "fc83f0176bc00783c0098945fc8b45fcc1e8058b4dfcc1e11b09c88945fc8b45f483c0"
      "ff8945f483f8007402ebc5eb308b45fc83c0116b4df80d31c88945fc8b45fcc1e00703"
      "45f83345fc8945fc8b45f483c0ff8945f483f8007402eb95eb008b45fc3345f883c003"
      "5dc3");
}

TEST_F(BinaryDiffSDK, CommonUpdatePagesEveryGuardAndRechecksFreshSource) {
  // Owned branch_progress_fixture.c family6, clang x64 ELF O2. Inspect only.
  checkBranchProgress(
      "89f183e1078d04768d1486eb1e0f1f0083f7178d04fd0000000029f883c009c1c01b83"
      "f9027c1dffc989c740f6c70175df83c71131d789f8c1e00701f031f883f9017fe331f0"
      "83c003c3");
}

TEST_F(BinaryDiffSDK, ModularProgressPagesAndAuthenticatesProofKind) {
  // Owned modular_progress_fixture.c, clang x64 ELF O0. Inspect only.
  checkBranchProgress(
      "554889e5897dfc8975f88b45f88945f48b45fc83e00183f80074228b45fc83f0176b"
      "c00783c0098945fc8b45fcc1e8058b4dfcc1e11b09c88945fceb1e8b45fc83c0116b"
      "4df40d31c88945fc8b45fcc1e0070345f83345fc8945fc8b45f483c0038945f4837d"
      "f40173a68b45fc3345f883c0035dc3",
      5, 1, "modular");
  // Same v5 API also authenticates the preferred monotone proof.
  checkBranchProgress(
      "89f183e1078d04768d1486eb1e0f1f0083f7178d04fd0000000029f883c009c1c01b83"
      "f9027c1dffc989c740f6c70175df83c71131d789f8c1e00701f031f883f9017fe331f0"
      "83c003c3",
      5, 2, "nowrap");
}

TEST_F(BinaryDiffSDK, ParameterBoundProgressPagesAndAuthenticatesProofKind) {
  // Owned parameter_bound_progress_fixture.c, clang x64 ELF O0. Inspect only.
  checkBranchProgress(
      "554889e5897dfc8975f88b45fc8945f48b45fc83e00183f80074228b45fc83f0176bc007"
      "83c0098945fc8b45fcc1e8058b4dfcc1e11b09c88945fceb1e8b45fc83c0116b4df40d"
      "31c88945fc8b45fcc1e0070345f83345fc8945fc8b45f483c0038945f48b45f48b4df8"
      "83c10039c8759f8b45fc3345f883c0035dc3",
      6, 1, "parameter_modular");
}

TEST_F(BinaryDiffSDK, InvariantBoundProgressPagesAndAuthenticatesProofKind) {
  // Owned invariant_bound_progress_fixture.c, clang x64 ELF O0. Inspect only.
  checkBranchProgress(
      "554889e5897dfc8975f88b45f88b4df8c1e90331c88b4dfc81e1ff00000001c88945"
      "f48b45fc8945f08b45fc83e00183f80074228b45fc83f0176bc00783c0098945fc8b"
      "45fcc1e8058b4dfcc1e11b09c88945fceb1e8b45fc83c0116b4df00d31c88945fc8b"
      "45fcc1e0070345f83345fc8945fc8b45f083c0038945f08b45f08b4df483c10039c8"
      "759f8b45fc3345f883c0035dc3",
      7, 1, "invariant_modular");
}

TEST_F(BinaryDiffSDK, ValidInvariantBoundProofStillRequiresFreshSourceBinding) {
  const auto Bytes = llvm::fromHex(
      "554889e5897dfc8975f88b45f88b4df8c1e90331c88b4dfc81e1ff00000001c88945"
      "f48b45fc8945f08b45fc83e00183f80074228b45fc83f0176bc00783c0098945fc8b"
      "45fcc1e8058b4dfcc1e11b09c88945fceb1e8b45fc83c0116b4df00d31c88945fc8b"
      "45fcc1e0070345f83345fc8945fc8b45f083c0038945f08b45f08b4df483c10039c8"
      "759f8b45fc3345f883c0035dc3");
  const auto Raw = llvm::ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(Bytes.data()), Bytes.size());
  const auto A = input(Raw, 0x1000), B = input(Raw, 0x8000);
  const auto Old = compare(
      A, B,
      R"({"discover":false,"dataflow":{"profile":"llvm-progress-ssa-v6"}})");
  const auto OldPage = parsed(neverd_comparison_dataflow_json(
      Old, R"({"kind":"nodes","address":"0x1000"})"));
  EXPECT_EQ(OldPage.getAsObject()->getObject("progress")->getString("kind"),
            "unproven");
  const auto C = compare(
      A, B,
      R"({"discover":false,"dataflow":{"profile":"llvm-progress-ssa-v7"}})");
  const auto Saved = take(neverd_comparison_export_json(C));
  auto V = llvm::json::parse(Saved);
  ASSERT_TRUE(bool(V));
  auto *Graph = V->getAsObject()
                    ->getObject("dataflow")
                    ->getObject("primary")
                    ->getArray("entries")
                    ->front()
                    .getAsObject()
                    ->getObject("graph");
  auto &Nodes = *Graph->getArray("nodes");
  ASSERT_EQ(Graph->getObject("progress")
                ->getArray("loops")
                ->front()
                .getAsObject()
                ->getString("proof"),
            "invariant_modular");
  bool Changed = false;
  for (auto &Node : Nodes)
    if (Node.getAsObject()->getString("opcode") == "and")
      for (auto &Input : *Node.getAsObject()->getArray("inputs")) {
        auto &Value = *Input.getAsArray();
        if (Value[0].getAsInteger() == 1 && Value[3].getAsString() == "0xff") {
          Value[3] = "0x7f";
          Changed = true;
        }
      }
  ASSERT_TRUE(Changed);
  // Changing the input mask preserves the invariant proof. Fresh native
  // binding, not mathematical progress alone, must reject the alteration.
  EXPECT_EQ(neverd_comparison_restore_json(C, render(*V).c_str()), 0);
  EXPECT_NE(take(neverd_comparison_last_error(C))
                .find("current native SSA projection"),
            std::string::npos);
  EXPECT_EQ(take(neverd_comparison_export_json(C)), Saved);
  EXPECT_EQ(neverd_comparison_restore_json(C, Saved.c_str()), 1);
}

TEST_F(BinaryDiffSDK, ValidParameterBoundProofStillRequiresFreshSourceBinding) {
  const auto Bytes = llvm::fromHex(
      "554889e5897dfc8975f88b45fc8945f48b45fc83e00183f80074228b45fc83f0176bc007"
      "83c0098945fc8b45fcc1e8058b4dfcc1e11b09c88945fceb1e8b45fc83c0116b4df40d"
      "31c88945fc8b45fcc1e0070345f83345fc8945fc8b45f483c0038945f48b45f48b4df8"
      "83c10039c8759f8b45fc3345f883c0035dc3");
  const auto Raw = llvm::ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(Bytes.data()), Bytes.size());
  const auto A = input(Raw, 0x1000), B = input(Raw, 0x8000);
  const auto Old = compare(
      A, B,
      R"({"discover":false,"dataflow":{"profile":"llvm-progress-ssa-v5"}})");
  const auto OldPage = parsed(neverd_comparison_dataflow_json(
      Old, R"({"kind":"nodes","address":"0x1000"})"));
  EXPECT_EQ(OldPage.getAsObject()->getObject("progress")->getString("kind"),
            "unproven");
  const auto C = compare(
      A, B,
      R"({"discover":false,"dataflow":{"profile":"llvm-progress-ssa-v6"}})");
  const auto Saved = take(neverd_comparison_export_json(C));
  auto V = llvm::json::parse(Saved);
  ASSERT_TRUE(bool(V));
  auto *Graph = V->getAsObject()
                    ->getObject("dataflow")
                    ->getObject("primary")
                    ->getArray("entries")
                    ->front()
                    .getAsObject()
                    ->getObject("graph");
  auto &Nodes = *Graph->getArray("nodes");
  const auto *Loop =
      Graph->getObject("progress")->getArray("loops")->front().getAsObject();
  ASSERT_EQ(Loop->getString("proof"), "parameter_modular");
  auto Replacement = Nodes[*Loop->getInteger("counter")]
                         .getAsObject()
                         ->getArray("inputs")
                         ->front();
  ASSERT_EQ((*Replacement.getAsArray())[0].getAsInteger(), 2);
  ASSERT_EQ((*Replacement.getAsArray())[3].getAsString(), "0x0");
  (*Replacement.getAsArray())[6] = nullptr;
  auto &Inputs =
      *Nodes[*Loop->getInteger("compare")].getAsObject()->getArray("inputs");
  ASSERT_EQ((*Inputs[1].getAsArray())[3].getAsString(), "0x1");
  Inputs[1] = std::move(Replacement);
  // The changed target still gives a valid relative exit proof. Only binding
  // to the original native graph can reject this replacement parameter.
  EXPECT_EQ(neverd_comparison_restore_json(C, render(*V).c_str()), 0);
  EXPECT_NE(take(neverd_comparison_last_error(C))
                .find("current native SSA projection"),
            std::string::npos);
  EXPECT_EQ(take(neverd_comparison_export_json(C)), Saved);
  EXPECT_EQ(neverd_comparison_restore_json(C, Saved.c_str()), 1);
}

TEST_F(BinaryDiffSDK, RestoresPhiNumberingWhileAuthenticatingEverySourceFact) {
  // Owned family6 x64 O2 has two header PHIs whose placement may change when
  // native SSA construction changes. The saved node IDs remain authoritative.
  const auto Bytes = llvm::fromHex(
      "89f183e1078d04768d1486eb1e0f1f0083f7178d04fd0000000029f883c009c1c01b83"
      "f9027c1dffc989c740f6c70175df83c71131d789f8c1e00701f031f883f9017fe331f0"
      "83c003c3");
  const auto Raw = llvm::ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(Bytes.data()), Bytes.size());
  const auto A = input(Raw, 0x1000), B = input(Raw, 0x8000);
  for (unsigned Version : {4, 5, 6, 7}) {
    const auto Options = render(llvm::json::Object{
        {"discover", false},
        {"dataflow",
         llvm::json::Object{
             {"profile", "llvm-progress-ssa-v" + std::to_string(Version)}}}});
    const auto C = compare(A, B, Options.c_str());
    const auto Original = take(neverd_comparison_export_json(C));
    auto Document = llvm::json::parse(Original);
    ASSERT_TRUE(bool(Document));
    for (const auto *Side : {"primary", "secondary"}) {
      auto *Graph = Document->getAsObject()
                        ->getObject("dataflow")
                        ->getObject(Side)
                        ->getArray("entries")
                        ->front()
                        .getAsObject()
                        ->getObject("graph");
      auto &Nodes = *Graph->getArray("nodes");
      int64_t Left = -1, Right = -1;
      for (size_t I = 0; I < Nodes.size() && Right < 0; ++I)
        if (Nodes[I].getAsObject()->getBoolean("phi").value_or(false))
          for (size_t J = I + 1; J < Nodes.size(); ++J)
            if (Nodes[J].getAsObject()->getBoolean("phi").value_or(false) &&
                Nodes[I].getAsObject()->getInteger("block") ==
                    Nodes[J].getAsObject()->getInteger("block")) {
              Left = I;
              Right = J;
              break;
            }
      ASSERT_GE(Left, 0);
      ASSERT_GE(Right, 0);
      const auto Remap = [&](int64_t Id) {
        return Id == Left ? Right : Id == Right ? Left : Id;
      };
      std::swap(Nodes[Left], Nodes[Right]);
      for (auto &Node : Nodes)
        for (auto &Value : *Node.getAsObject()->getArray("inputs")) {
          auto &Input = *Value.getAsArray();
          if (Input[0].getAsInteger() == 0)
            Input[1] = Remap(*Input[1].getAsInteger());
        }
      for (auto &Block : *Graph->getArray("blocks"))
        if (const auto Id = Block.getAsObject()->getInteger("terminator"))
          (*Block.getAsObject())["terminator"] = Remap(*Id);
      for (auto &Loop : *Graph->getObject("progress")->getArray("loops"))
        for (const auto *Key : {"counter", "update", "compare"})
          (*Loop.getAsObject())[Key] =
              Remap(*Loop.getAsObject()->getInteger(Key));
    }
    ASSERT_EQ(neverd_comparison_restore_json(C, render(*Document).c_str()), 1)
        << take(neverd_comparison_last_error(C));
    const auto Canonical = take(neverd_comparison_export_json(C));
    EXPECT_NE(Canonical, Original);
    auto Restored = llvm::json::parse(Canonical);
    ASSERT_TRUE(bool(Restored));
    EXPECT_EQ(*Restored, *Document);
    ASSERT_EQ(neverd_comparison_restore_json(C, Canonical.c_str()), 1);
    EXPECT_EQ(take(neverd_comparison_export_json(C)), Canonical);
    auto *Nodes = Restored->getAsObject()
                      ->getObject("dataflow")
                      ->getObject("primary")
                      ->getArray("entries")
                      ->front()
                      .getAsObject()
                      ->getObject("graph")
                      ->getArray("nodes");
    bool Altered = false;
    for (auto &Node : *Nodes) {
      auto *Origins = Node.getAsObject()->getArray("origins");
      if (!Origins->empty()) {
        (*Origins->front().getAsObject())["sequence"] = 999;
        Altered = true;
        break;
      }
    }
    ASSERT_TRUE(Altered);
    EXPECT_EQ(neverd_comparison_restore_json(C, render(*Restored).c_str()), 0);
    EXPECT_EQ(take(neverd_comparison_export_json(C)), Canonical);
  }
}

TEST_F(BinaryDiffSDK, RestoreCancellationRetainsEditsHistoryAndInspection) {
  auto A = input(body()), B = input(body(9), 0x8000);
  const auto C = compare(
      A, B,
      R"({"discover":false,"dataflow":true,"regions":true,"flow_regions":true})");
  const auto Saved = take(neverd_comparison_export_json(C));
  ASSERT_EQ(
      neverd_comparison_override_json(
          C, R"({"kind":"confirm","primary":"0x1000","secondary":"0x8000"})"),
      1);
  const auto Edited = take(neverd_comparison_export_json(C));
  ASSERT_NE(Edited, Saved);
  const auto Summary = take(neverd_comparison_summary_json(C));
  const auto Detail = take(neverd_comparison_function_json(C, 0, 0x1000));
  ASSERT_FALSE(Detail.empty());
  struct Probe {
    neverd_comparison_t Handle;
    const char *Stage;
    std::thread::id Caller = std::this_thread::get_id();
    bool Seen = false;
    static void report(void *User, const char *Phase, neverd_va_t,
                       neverd_va_t) {
      auto &P = *static_cast<Probe *>(User);
      EXPECT_EQ(std::this_thread::get_id(), P.Caller);
      const auto Status = parsed(neverd_comparison_status_json(P.Handle));
      EXPECT_EQ(Status.getAsObject()->getBoolean("running"), true);
      EXPECT_EQ(Status.getAsObject()->getString("restore_state"), "running");
      EXPECT_EQ(Status.getAsObject()->getBoolean("previous_result_retained"),
                true);
      if (llvm::StringRef(Phase) == P.Stage) {
        P.Seen = true;
        neverd_comparison_cancel(P.Handle);
      }
    }
  };
  for (const auto *Stage :
       {"restore:scan", "restore:parse", "restore:records", "restore:identity",
        "restore:decisions", "restore:dataflow", "restore:reviews",
        "restore:regions", "restore:flow_regions", "restore:measure",
        "restore:publish"}) {
    SCOPED_TRACE(Stage);
    Probe P{C, Stage};
    EXPECT_EQ(neverd_comparison_restore_json_with_progress(C, Saved.c_str(),
                                                           Probe::report, &P),
              0);
    EXPECT_TRUE(P.Seen);
    const auto Status = parsed(neverd_comparison_status_json(C));
    EXPECT_EQ(Status.getAsObject()->getString("restore_state"), "cancelled");
    EXPECT_EQ(Status.getAsObject()->getBoolean("previous_result_retained"),
              true);
    EXPECT_EQ(Status.getAsObject()->getBoolean("partial_result_retained"),
              false);
    EXPECT_EQ(Status.getAsObject()->getBoolean("running"), false);
    EXPECT_EQ(take(neverd_comparison_export_json(C)), Edited);
    EXPECT_EQ(take(neverd_comparison_summary_json(C)), Summary);
    EXPECT_EQ(take(neverd_comparison_function_json(C, 0, 0x1000)), Detail);
  }
  // A malformed document follows the same retention contract and does not
  // consume an existing edit or redo record.
  EXPECT_EQ(neverd_comparison_restore_json(C, "{}"), 0);
  auto Status = parsed(neverd_comparison_status_json(C));
  EXPECT_EQ(Status.getAsObject()->getString("restore_state"), "failed");
  EXPECT_EQ(take(neverd_comparison_export_json(C)), Edited);
  auto Contradictory = llvm::json::parse(Saved);
  ASSERT_TRUE(bool(Contradictory));
  Contradictory->getAsObject()
      ->getArray("overrides")
      ->push_back(llvm::json::Object{
          {"kind", "unmatch"}, {"primary", "0x1000"}, {"secondary", "0x8000"}});
  EXPECT_EQ(neverd_comparison_restore_json(C, render(*Contradictory).c_str()),
            0);
  EXPECT_NE(take(neverd_comparison_last_error(C)).find("effective unmatch"),
            std::string::npos);
  EXPECT_EQ(take(neverd_comparison_export_json(C)), Edited);
  EXPECT_EQ(take(neverd_comparison_summary_json(C)), Summary);
  EXPECT_EQ(take(neverd_comparison_function_json(C, 0, 0x1000)), Detail);
  ASSERT_EQ(neverd_comparison_undo(C), 1);
  EXPECT_EQ(take(neverd_comparison_export_json(C)), Saved);
  ASSERT_EQ(neverd_comparison_redo(C), 1);
  EXPECT_EQ(take(neverd_comparison_export_json(C)), Edited);
  neverd_comparison_cancel(C);
  EXPECT_EQ(neverd_comparison_restore_json(C, Saved.c_str()), 1);
  EXPECT_EQ(take(neverd_comparison_export_json(C)), Saved);
  Status = parsed(neverd_comparison_status_json(C));
  EXPECT_EQ(Status.getAsObject()->getString("restore_state"), "complete");
  EXPECT_EQ(Status.getAsObject()->getBoolean("previous_result_retained"),
            false);
  EXPECT_EQ(parsed(neverd_comparison_summary_json(C))
                .getAsObject()
                ->getBoolean("can_undo"),
            false);
}

TEST_F(BinaryDiffSDK, RestoreCompletionIsIndependentOfTheSavedStopState) {
  auto A = input(body()), B = input(body(), 0x8000);
  const auto C = compare(A, B);
  const auto Saved = take(neverd_comparison_export_json(C));
  for (const auto *State :
       {"complete", "cancelled", "timed_out", "resource_limited"}) {
    SCOPED_TRACE(State);
    auto Document = llvm::json::parse(Saved);
    ASSERT_TRUE(bool(Document));
    (*Document->getAsObject())["state"] = State;
    (*Document->getAsObject())["stopped_phase"] =
        llvm::StringRef(State) == "complete" ? "" : "measure";
    // This historical setting is not a deadline for the restore operation.
    (*Document->getAsObject()->getObject("settings"))["timeout_ms"] = 1;
    EXPECT_EQ(neverd_comparison_restore_json(C, render(*Document).c_str()), 1)
        << take(neverd_comparison_last_error(C));
    auto Status = parsed(neverd_comparison_status_json(C));
    EXPECT_EQ(Status.getAsObject()->getString("restore_state"), "complete");
    EXPECT_EQ(Status.getAsObject()->getString("state"), State);
    EXPECT_EQ(Status.getAsObject()->getBoolean("previous_result_retained"),
              false);
    EXPECT_EQ(parsed(neverd_comparison_export_json(C))
                  .getAsObject()
                  ->getString("state"),
              State);
  }
}
