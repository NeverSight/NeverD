//===- SBFIntegrationTests.cpp - public API Solana SBF tests ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "SBFFixtureBuilder.h"
#include "gtest/gtest.h"

#include "neverd/sbf/solana/SBFAnchor.h"
#include "neverd/sdk/NeverDCAPI.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::string takeString(const char *Text) {
  if (!Text)
    return {};
  std::string Copy(Text);
  neverd_free_string(Text);
  return Copy;
}

std::array<uint8_t, neverd::sbf::kInstructionSize>
encode(neverd::sbf::Opcode ID, uint8_t Dst = 0, uint8_t Src = 0,
       int16_t Offset = 0, int32_t Immediate = 0) {
  std::array<uint8_t, neverd::sbf::kInstructionSize> Bytes{};
  const neverd::sbf::OpcodeInfo *Info = neverd::sbf::getOpcodeInfo(ID);
  EXPECT_NE(Info, nullptr);
  if (!Info)
    return Bytes;
  Bytes[neverd::sbf::kOpcodeOffset] = Info->Encoding;
  Bytes[neverd::sbf::kRegisterByteOffset] =
      static_cast<uint8_t>((Src << neverd::sbf::kRegisterEncodingBits) | Dst);
  llvm::support::endian::write16le(Bytes.data() +
                                       neverd::sbf::kBranchOffsetOffset,
                                   static_cast<uint16_t>(Offset));
  llvm::support::endian::write32le(Bytes.data() + neverd::sbf::kImmediateOffset,
                                   static_cast<uint32_t>(Immediate));
  return Bytes;
}

void append(std::vector<uint8_t> &Text,
            const std::array<uint8_t, neverd::sbf::kInstructionSize> &Bytes) {
  Text.insert(Text.end(), Bytes.begin(), Bytes.end());
}

/// Append a 64-bit constant load, which occupies two instruction slots.
void appendLoadImm64(std::vector<uint8_t> &Text, uint8_t Dst, uint64_t Value) {
  append(Text, encode(neverd::sbf::Opcode::LDDW, Dst, /*Src=*/0, /*Offset=*/0,
                      static_cast<int32_t>(Value)));
  std::array<uint8_t, neverd::sbf::kInstructionSize> High{};
  llvm::support::endian::write32le(High.data() + neverd::sbf::kImmediateOffset,
                                   static_cast<uint32_t>(Value >> 32));
  append(Text, High);
}

class SBFIntegrationTest : public ::testing::Test {
protected:
  void SetUp() override {
    llvm::SmallString<128> UniquePath;
    const std::error_code EC =
        llvm::sys::fs::createTemporaryFile("neverd-api-sbf", "so", UniquePath);
    ASSERT_FALSE(EC) << EC.message();
    Path = std::filesystem::path(UniquePath.str().str());
    Session = neverd_session_create();
  }

  void TearDown() override {
    neverd_session_destroy(Session);
    std::error_code EC;
    std::filesystem::remove(Path, EC);
  }

  void write(const std::vector<uint8_t> &Bytes) const {
    std::ofstream Output(Path, std::ios::binary);
    Output.write(reinterpret_cast<const char *>(Bytes.data()),
                 static_cast<std::streamsize>(Bytes.size()));
  }

  std::filesystem::path Path;
  neverd_session_t Session = nullptr;
};

TEST_F(SBFIntegrationTest, ExposesAllStagesAndBothSourceLanguages) {
  neverd::sbf::test::StrictELFOptions Options;
  Options.TheVersion = neverd::sbf::Version::V3;
  Options.AddRodata = true;
  write(neverd::sbf::test::buildStrictELF(Options));

  ASSERT_EQ(neverd_session_load(Session, Path.string().c_str()), 1)
      << takeString(neverd_last_error(Session));
  EXPECT_EQ(takeString(neverd_session_arch_name(Session)), "sbf");
  EXPECT_EQ(takeString(neverd_session_format_name(Session)), "ELF");
  EXPECT_EQ(neverd_session_bitness(Session), 64);
  ASSERT_EQ(neverd_func_count(Session), 1);
  EXPECT_EQ(takeString(neverd_func_name(Session, 0)), "entrypoint");

  auto Headers = llvm::json::parse(takeString(neverd_headers_json(Session)));
  ASSERT_TRUE(static_cast<bool>(Headers));
  const llvm::json::Object *Root = Headers->getAsObject();
  ASSERT_NE(Root, nullptr);
  const llvm::json::Object *SBF = Root->getObject("sbf");
  ASSERT_NE(SBF, nullptr);
  EXPECT_EQ(SBF->getString("version"), "v3");
  EXPECT_EQ(SBF->getInteger("machine"), neverd::sbf::kELFMachineBPF);
  EXPECT_EQ(SBF->getString("machine_name"), "EM_BPF");
  EXPECT_EQ(SBF->getString("layout"), "strict-program-header");
  EXPECT_EQ(SBF->getString("debug_enrichment"), "unavailable");

  ASSERT_EQ(neverd_session_analyze(Session), 1)
      << takeString(neverd_last_error(Session));
  EXPECT_NE(takeString(neverd_ir_low(Session, 0)).find("SBF LowIR SBF v3"),
            std::string::npos);
  EXPECT_NE(takeString(neverd_ir_med(Session, 0)).find("SBF MedIR SBF v3"),
            std::string::npos);
  EXPECT_NE(takeString(neverd_ir_high(Session, 0)).find("entrypoint"),
            std::string::npos);
  EXPECT_NE(takeString(neverd_ir_llvm(Session, 0)).find("@neverd_sbf_program"),
            std::string::npos);
  EXPECT_NE(
      takeString(neverd_disasm_json(Session, neverd::sbf::kBytecodeStart, 10))
          .find("exit"),
      std::string::npos);
  EXPECT_NE(takeString(neverd_cfg_json(Session, neverd::sbf::kBytecodeStart))
                .find("entrypoint"),
            std::string::npos);

  const std::string C = takeString(neverd_decompile_all_ex(
      Session, Path.string().c_str(), NEVERD_OUTPUT_C, 0, 0));
  EXPECT_NE(C.find("neverd_sbf_program"), std::string::npos);
  const std::string Rust = takeString(neverd_decompile_all_ex(
      Session, Path.string().c_str(), NEVERD_OUTPUT_RUST, 0, 0));
  EXPECT_NE(Rust.find("pub fn neverd_sbf_program"), std::string::npos);
  EXPECT_NE(takeString(neverd_lift_module(Session, Path.string().c_str(), 0, 0))
                .find("@neverd_sbf_program"),
            std::string::npos);
}

TEST_F(SBFIntegrationTest, RejectsInvalidConfigurationAndPatchRoutes) {
  write(neverd::sbf::test::buildStrictELF());
  ASSERT_EQ(neverd_session_load(Session, Path.string().c_str()), 1);
  EXPECT_EQ(neverd_sbf_set_version(Session, "future"), 0);
  EXPECT_NE(takeString(neverd_last_error(Session)).find("unknown SBF version"),
            std::string::npos);
  EXPECT_EQ(neverd_sbf_set_version(Session, "auto"), 1);
  neverd_sbf_set_strict(Session, 1);
  EXPECT_EQ(neverd_lift_to_obj(Session, Path.string().c_str(), 0, 0), 1);
  EXPECT_NE(takeString(neverd_last_error(Session))
                .find("object-code roundtrip is not supported"),
            std::string::npos);
}

TEST_F(SBFIntegrationTest, SuppliedIDLNamesADispatchArmTheDictionaryCannot) {
  // A name the built-in dictionary does not carry, so the supplied IDL is the
  // only thing that can account for the discriminator the program compares.
  constexpr llvm::StringLiteral Handler("record_trade_v2");
  const neverd::sbf::AnchorDiscriminator Discriminator =
      neverd::sbf::anchorDiscriminator(
          neverd::sbf::AnchorNamespace::Instruction, Handler);

  neverd::sbf::test::StrictELFOptions Options;
  appendLoadImm64(Options.Text, /*Dst=*/6, Discriminator.toWord());
  append(Options.Text, encode(neverd::sbf::Opcode::JEQ64_REG, /*Dst=*/7,
                              /*Src=*/6, /*Offset=*/1));
  append(Options.Text, encode(neverd::sbf::Opcode::EXIT));
  append(Options.Text, encode(neverd::sbf::Opcode::EXIT));
  write(neverd::sbf::test::buildStrictELF(Options));

  ASSERT_EQ(neverd_session_load(Session, Path.string().c_str()), 1)
      << takeString(neverd_last_error(Session));

  // A document that cannot be trusted has to be refused with a reason, so that
  // recovery never silently reports names from a half-read IDL.
  EXPECT_EQ(neverd_sbf_set_idl(Session, "{"), 0);
  EXPECT_FALSE(takeString(neverd_last_error(Session)).empty());
  EXPECT_EQ(neverd_sbf_set_idl(Session, R"({"address": "not-an-address"})"), 0);
  EXPECT_NE(takeString(neverd_last_error(Session)).find("base58"),
            std::string::npos);

  const std::string Document =
      R"({"metadata": {"name": "ledger"}, "instructions": [{"name": ")" +
      Handler.str() + R"("}]})";
  ASSERT_EQ(neverd_sbf_set_idl(Session, Document.c_str()), 1)
      << takeString(neverd_last_error(Session));

  ASSERT_EQ(neverd_session_analyze(Session), 1)
      << takeString(neverd_last_error(Session));
  const std::string Named = takeString(neverd_ir_high(Session, 0));
  EXPECT_NE(Named.find(Handler.str()), std::string::npos) << Named;
  EXPECT_NE(Named.find("idl=ledger"), std::string::npos) << Named;

  // Clearing returns the session to the built-in dictionary, which has no name
  // for this discriminator. With nothing left to prove the comparison is a
  // dispatch, recovery has to withdraw the claim rather than keep the name or
  // guess at an unnamed arm.
  ASSERT_EQ(neverd_sbf_set_idl(Session, nullptr), 1)
      << takeString(neverd_last_error(Session));
  const std::string Unnamed = takeString(neverd_ir_high(Session, 0));
  EXPECT_EQ(Unnamed.find(Handler.str()), std::string::npos) << Unnamed;
  EXPECT_EQ(Unnamed.find("framework anchor"), std::string::npos) << Unnamed;
}

TEST_F(SBFIntegrationTest, DisassemblyHonorsRecoveredFunctionBoundaries) {
  neverd::sbf::test::StrictELFOptions Options;
  Options.EntrySlot = 2;
  append(Options.Text,
         encode(neverd::sbf::Opcode::MOV64_IMM, /*Dst=*/0, /*Src=*/0,
                /*Offset=*/0, /*Immediate=*/42));
  append(Options.Text, encode(neverd::sbf::Opcode::EXIT));
  append(Options.Text,
         encode(neverd::sbf::Opcode::CALL_IMM, /*Dst=*/0, /*Src=*/1,
                /*Offset=*/0, /*Immediate=*/-3));
  append(Options.Text, encode(neverd::sbf::Opcode::EXIT));
  write(neverd::sbf::test::buildStrictELF(Options));

  ASSERT_EQ(neverd_session_load(Session, Path.string().c_str()), 1)
      << takeString(neverd_last_error(Session));

  auto Headers = llvm::json::parse(takeString(neverd_headers_json(Session)));
  ASSERT_TRUE(static_cast<bool>(Headers));
  const llvm::json::Object *HeaderRoot = Headers->getAsObject();
  ASSERT_NE(HeaderRoot, nullptr);
  EXPECT_EQ(HeaderRoot->getInteger("func_count"), 2);

  auto Dashboard =
      llvm::json::parse(takeString(neverd_dashboard_json(Session)));
  ASSERT_TRUE(static_cast<bool>(Dashboard));
  const llvm::json::Object *DashboardRoot = Dashboard->getAsObject();
  ASSERT_NE(DashboardRoot, nullptr);
  const llvm::json::Object *Counts = DashboardRoot->getObject("counts");
  ASSERT_NE(Counts, nullptr);
  EXPECT_EQ(Counts->getInteger("functions"), 2);

  EXPECT_EQ(neverd_func_count(Session), 2);
  EXPECT_GE(neverd_func_find_by_addr(Session, neverd::sbf::kBytecodeStart), 0);

  auto CallGraph =
      llvm::json::parse(takeString(neverd_callgraph_json(Session)));
  ASSERT_TRUE(static_cast<bool>(CallGraph));
  const llvm::json::Object *CallGraphRoot = CallGraph->getAsObject();
  ASSERT_NE(CallGraphRoot, nullptr);
  ASSERT_NE(CallGraphRoot->getArray("nodes"), nullptr);
  EXPECT_EQ(CallGraphRoot->getArray("nodes")->size(), 2u);
  ASSERT_NE(CallGraphRoot->getArray("edges"), nullptr);
  EXPECT_EQ(CallGraphRoot->getArray("edges")->size(), 1u);

  auto Resolved = llvm::json::parse(
      takeString(neverd_resolve_addr(Session, neverd::sbf::kBytecodeStart)));
  ASSERT_TRUE(static_cast<bool>(Resolved));
  ASSERT_NE(Resolved->getAsObject(), nullptr);
  EXPECT_EQ(Resolved->getAsObject()->getString("type"), "function");

  const std::string Text = takeString(neverd_disasm_text(
      Session, neverd::sbf::kEntrySymbolName.data(), /*Annotate=*/0));
  EXPECT_NE(Text.find("call "), std::string::npos);
  EXPECT_EQ(Text.find("mov64"), std::string::npos);

  auto JSON = llvm::json::parse(
      takeString(neverd_disasm_json(Session, neverd::sbf::kBytecodeStart,
                                    /*MaxInsns=*/0)));
  ASSERT_TRUE(static_cast<bool>(JSON));
  const llvm::json::Array *Instructions = JSON->getAsArray();
  ASSERT_NE(Instructions, nullptr);
  EXPECT_EQ(Instructions->size(), 2u);

  auto CFG = llvm::json::parse(takeString(
      neverd_cfg_json(Session, neverd::sbf::kBytecodeStart +
                                   2 * neverd::sbf::kInstructionSize)));
  ASSERT_TRUE(static_cast<bool>(CFG));
  const llvm::json::Object *CFGRoot = CFG->getAsObject();
  ASSERT_NE(CFGRoot, nullptr);
  const llvm::json::Array *Edges = CFGRoot->getArray("edges");
  ASSERT_NE(Edges, nullptr);
  ASSERT_EQ(Edges->size(), 1u);
  ASSERT_NE((*Edges)[0].getAsObject(), nullptr);
  EXPECT_EQ((*Edges)[0].getAsObject()->getString("type").value_or(""),
            "fallthrough");

  const std::string DOT = takeString(neverd_cfg_dot(
      Session, Path.string().c_str(), neverd::sbf::kEntrySymbolName.data(),
      /*Styled=*/0));
  EXPECT_EQ(DOT.find("bb0 "), std::string::npos);
  EXPECT_NE(DOT.find("bb1 "), std::string::npos);
}

} // namespace
