//===- SBFIntegrationTests.cpp - public API Solana SBF tests ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "SBFFixtureBuilder.h"
#include "gtest/gtest.h"

#include "neverd/sbf/analysis/SBFAnalysisLimits.h"
#include "neverd/sbf/image/SBFProgramImage.h"
#include "neverd/sbf/runtime/SBFSyscalls.h"
#include "neverd/sbf/solana/SBFAnchor.h"
#include "neverd/sdk/NeverDCAPI.h"
#include "neverd/support/BinaryLoading.h"
#include "neverd/support/TextEncoding.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
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

TEST_F(SBFIntegrationTest,
       ExplicitV4SelectionUsesTheUpstreamOfflineVersionRange) {
  neverd::sbf::test::StrictELFOptions Options;
  Options.TheVersion = neverd::sbf::Version::V4;
  write(neverd::sbf::test::buildStrictELF(Options));

  ASSERT_EQ(neverd_session_load(Session, Path.string().c_str()), 1)
      << takeString(neverd_last_error(Session));

  // Auto-detection answers the chain-profile question. Agave currently caps
  // that environment at v3, so a v4 program must not be reported as runnable.
  EXPECT_EQ(neverd_session_analyze(Session), 0);
  EXPECT_NE(
      takeString(neverd_last_error(Session)).find("enabled version range"),
      std::string::npos);

  // An explicit version answers a different, offline-tooling question. It
  // must expose every ISA version supported by the pinned upstream sbpf while
  // preserving invalid instructions as fault nodes in relaxed mode.
  const std::string V4Name =
      neverd::sbf::versionName(neverd::sbf::Version::V4).str();
  ASSERT_EQ(neverd_sbf_set_version(Session, V4Name.c_str()), 1)
      << takeString(neverd_last_error(Session));
  ASSERT_EQ(neverd_session_analyze(Session), 1)
      << takeString(neverd_last_error(Session));

  neverd_sbf_set_strict(Session, 0);
  ASSERT_EQ(neverd_session_analyze(Session), 1)
      << takeString(neverd_last_error(Session));
  const std::string VersionHeading =
      (llvm::Twine("SBF LowIR ") +
       neverd::sbf::versionDisplayName(neverd::sbf::Version::V4))
          .str();
  EXPECT_NE(takeString(neverd_ir_low(Session, 0)).find(VersionHeading),
            std::string::npos);

  const std::string Decompiled = takeString(neverd_decompile_all_ex(
      Session, Path.string().c_str(), NEVERD_OUTPUT_C, 0, 0));
  EXPECT_NE(Decompiled.find("neverd_sbf_program"), std::string::npos)
      << takeString(neverd_last_error(Session));
}

TEST_F(SBFIntegrationTest, CFGJSONPreservesTerminalFaultEdges) {
  neverd::sbf::test::StrictELFOptions Options;
  Options.TheVersion = neverd::sbf::Version::V3;
  Options.EntrySlot = 1;
  appendLoadImm64(Options.Text, /*Dst=*/0, /*Value=*/0);
  append(Options.Text, encode(neverd::sbf::Opcode::EXIT));
  write(neverd::sbf::test::buildStrictELF(Options));

  ASSERT_EQ(neverd_session_load(Session, Path.string().c_str()), 1)
      << takeString(neverd_last_error(Session));
  ASSERT_EQ(neverd_session_analyze(Session), 1)
      << takeString(neverd_last_error(Session));
  auto CFG = llvm::json::parse(takeString(neverd_cfg_json(
      Session, neverd::sbf::kBytecodeStart + neverd::sbf::kInstructionSize)));
  ASSERT_TRUE(static_cast<bool>(CFG));
  const llvm::json::Object *Root = CFG->getAsObject();
  ASSERT_NE(Root, nullptr);
  const llvm::json::Array *Edges = Root->getArray("edges");
  ASSERT_NE(Edges, nullptr);
  ASSERT_EQ(Edges->size(), 1u);
  const llvm::json::Object *Edge = (*Edges)[0].getAsObject();
  ASSERT_NE(Edge, nullptr);
  EXPECT_EQ(Edge->getString("type"), "fault");
  ASSERT_NE(Edge->get("to"), nullptr);
  EXPECT_EQ(Edge->get("to")->kind(), llvm::json::Value::Null);
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

  (void)takeString(neverd_cfg_json(Session, /*FuncEntry=*/0));
  ASSERT_FALSE(takeString(neverd_last_error(Session)).empty());
  auto CallGraph =
      llvm::json::parse(takeString(neverd_callgraph_json(Session)));
  ASSERT_TRUE(static_cast<bool>(CallGraph));
  const llvm::json::Object *CallGraphRoot = CallGraph->getAsObject();
  ASSERT_NE(CallGraphRoot, nullptr);
  ASSERT_NE(CallGraphRoot->getArray("nodes"), nullptr);
  EXPECT_EQ(CallGraphRoot->getArray("nodes")->size(), 2u);
  ASSERT_NE(CallGraphRoot->getArray("edges"), nullptr);
  EXPECT_EQ(CallGraphRoot->getArray("edges")->size(), 1u);
  EXPECT_TRUE(takeString(neverd_last_error(Session)).empty())
      << "a successful call-graph query must clear a stale diagnostic";

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
  ASSERT_EQ(Edges->size(), 2u);
  const auto EdgeByType = [&](llvm::StringRef Type) {
    return std::find_if(Edges->begin(), Edges->end(), [&](const auto &Value) {
      const llvm::json::Object *Edge = Value.getAsObject();
      return Edge && Edge->getString("type") == Type;
    });
  };
  const auto Fallthrough = EdgeByType("fallthrough");
  ASSERT_NE(Fallthrough, Edges->end());
  ASSERT_NE(Fallthrough->getAsObject()->get("to"), nullptr);
  EXPECT_NE(Fallthrough->getAsObject()->get("to")->kind(),
            llvm::json::Value::Null);
  const auto Return = EdgeByType("return");
  ASSERT_NE(Return, Edges->end());
  ASSERT_NE(Return->getAsObject()->get("to"), nullptr);
  EXPECT_EQ(Return->getAsObject()->get("to")->kind(), llvm::json::Value::Null);

  const std::string DOT = takeString(neverd_cfg_dot(
      Session, Path.string().c_str(), neverd::sbf::kEntrySymbolName.data(),
      /*Styled=*/0));
  EXPECT_EQ(DOT.find("bb0 "), std::string::npos);
  EXPECT_NE(DOT.find("bb1 "), std::string::npos);
}

TEST_F(SBFIntegrationTest, SharedTailRemainsInEverySemanticFunctionBody) {
  constexpr size_t EntrySlot = 0;
  constexpr size_t SecondEntrySlot = 3;
  constexpr size_t SharedTailSlot = 5;
  constexpr size_t CalleeSlot = 7;
  constexpr size_t EntryInstructionCount = 4;
  constexpr size_t SecondInstructionCount = 3;

  neverd::sbf::test::StrictELFOptions Options;
  append(Options.Text,
         encode(neverd::sbf::Opcode::CALL_IMM, /*Dst=*/0, /*Src=*/1,
                /*Offset=*/0,
                static_cast<int32_t>(SecondEntrySlot - EntrySlot - 1)));
  append(Options.Text,
         encode(neverd::sbf::Opcode::JA, /*Dst=*/0, /*Src=*/0,
                static_cast<int16_t>(SharedTailSlot - EntrySlot - 2)));
  append(Options.Text, encode(neverd::sbf::Opcode::EXIT));
  append(Options.Text,
         encode(neverd::sbf::Opcode::JA, /*Dst=*/0, /*Src=*/0,
                static_cast<int16_t>(SharedTailSlot - SecondEntrySlot - 1)));
  append(Options.Text, encode(neverd::sbf::Opcode::EXIT));
  append(Options.Text,
         encode(neverd::sbf::Opcode::CALL_IMM, /*Dst=*/0, /*Src=*/1,
                /*Offset=*/0,
                static_cast<int32_t>(CalleeSlot - SharedTailSlot - 1)));
  append(Options.Text, encode(neverd::sbf::Opcode::EXIT));
  append(Options.Text, encode(neverd::sbf::Opcode::EXIT));
  write(neverd::sbf::test::buildStrictELF(Options));

  ASSERT_EQ(neverd_session_load(Session, Path.string().c_str()), 1)
      << takeString(neverd_last_error(Session));
  ASSERT_EQ(neverd_session_analyze(Session), 1)
      << takeString(neverd_last_error(Session));
  ASSERT_EQ(neverd_func_count(Session), 3);

  const neverd_va_t EntryAddress = neverd::sbf::kBytecodeStart;
  const neverd_va_t SecondAddress =
      EntryAddress + SecondEntrySlot * neverd::sbf::kInstructionSize;
  const neverd_va_t CalleeAddress =
      EntryAddress + CalleeSlot * neverd::sbf::kInstructionSize;
  const int EntryFunction = neverd_func_find_by_addr(Session, EntryAddress);
  const int SecondFunction = neverd_func_find_by_addr(Session, SecondAddress);
  ASSERT_GE(EntryFunction, 0);
  ASSERT_GE(SecondFunction, 0);
  EXPECT_EQ(
      neverd_func_size(Session, EntryFunction),
      static_cast<int>(EntryInstructionCount * neverd::sbf::kInstructionSize));
  EXPECT_EQ(
      neverd_func_size(Session, SecondFunction),
      static_cast<int>(SecondInstructionCount * neverd::sbf::kInstructionSize));

  auto EntryDisassembly = llvm::json::parse(
      takeString(neverd_disasm_json(Session, EntryAddress, /*MaxInsns=*/0)));
  auto SecondDisassembly = llvm::json::parse(
      takeString(neverd_disasm_json(Session, SecondAddress, /*MaxInsns=*/0)));
  ASSERT_TRUE(static_cast<bool>(EntryDisassembly));
  ASSERT_TRUE(static_cast<bool>(SecondDisassembly));
  ASSERT_NE(EntryDisassembly->getAsArray(), nullptr);
  ASSERT_NE(SecondDisassembly->getAsArray(), nullptr);
  EXPECT_EQ(EntryDisassembly->getAsArray()->size(), EntryInstructionCount);
  EXPECT_EQ(SecondDisassembly->getAsArray()->size(), SecondInstructionCount);

  auto CallGraph =
      llvm::json::parse(takeString(neverd_callgraph_json(Session)));
  ASSERT_TRUE(static_cast<bool>(CallGraph));
  const llvm::json::Object *CallGraphRoot = CallGraph->getAsObject();
  ASSERT_NE(CallGraphRoot, nullptr);
  const llvm::json::Array *Edges = CallGraphRoot->getArray("edges");
  ASSERT_NE(Edges, nullptr);
  std::set<std::pair<std::string, std::string>> Calls;
  for (const llvm::json::Value &Value : *Edges) {
    const llvm::json::Object *Edge = Value.getAsObject();
    ASSERT_NE(Edge, nullptr);
    const std::optional<llvm::StringRef> Caller = Edge->getString("caller");
    const std::optional<llvm::StringRef> Callee = Edge->getString("callee");
    ASSERT_TRUE(Caller.has_value());
    ASSERT_TRUE(Callee.has_value());
    Calls.emplace(Caller->str(), Callee->str());
  }
  const auto AddressString = [](neverd_va_t Address) {
    return (llvm::Twine("0x") + llvm::utohexstr(Address)).str();
  };
  EXPECT_TRUE(Calls.contains(
      {AddressString(EntryAddress), AddressString(SecondAddress)}));
  EXPECT_TRUE(Calls.contains(
      {AddressString(EntryAddress), AddressString(CalleeAddress)}));
  EXPECT_TRUE(Calls.contains(
      {AddressString(SecondAddress), AddressString(CalleeAddress)}));
}

TEST_F(SBFIntegrationTest,
       SynchronizesTenThousandFunctionsWithALargeSharedTailOnce) {
  constexpr size_t FunctionCount = 10'000;
  constexpr size_t SharedBlockCount = 10'000;
  constexpr size_t BlockCount = FunctionCount + SharedBlockCount;

  neverd::sbf::test::StrictELFOptions Options;
  Options.AddDebugSymbols = true;
  Options.Text.reserve(BlockCount * neverd::sbf::kInstructionSize);
  Options.FunctionSymbols.reserve(FunctionCount);
  for (size_t FunctionID = 0; FunctionID < FunctionCount; ++FunctionID) {
    const int16_t ToSharedTail =
        static_cast<int16_t>(FunctionCount - FunctionID - 1);
    append(Options.Text,
           encode(neverd::sbf::Opcode::JA, /*Dst=*/0, /*Src=*/0, ToSharedTail));
    Options.FunctionSymbols.push_back({"function_" + std::to_string(FunctionID),
                                       FunctionID,
                                       /*SlotCount=*/0});
  }
  for (size_t BlockID = 0; BlockID + 1 < SharedBlockCount; ++BlockID)
    append(Options.Text, encode(neverd::sbf::Opcode::JA, /*Dst=*/0, /*Src=*/0,
                                /*Offset=*/0));
  append(Options.Text, encode(neverd::sbf::Opcode::EXIT));
  write(neverd::sbf::test::buildStrictELF(Options));

  ASSERT_EQ(neverd_session_load(Session, Path.string().c_str()), 1)
      << takeString(neverd_last_error(Session));
  ASSERT_EQ(neverd_session_analyze(Session), 1)
      << takeString(neverd_last_error(Session));
  ASSERT_EQ(neverd_func_count(Session), static_cast<int>(FunctionCount));

  const int ExpectedSize =
      static_cast<int>((SharedBlockCount + 1) * neverd::sbf::kInstructionSize);
  EXPECT_EQ(neverd_func_size(Session, 0), ExpectedSize);
  EXPECT_EQ(neverd_func_size(Session, static_cast<int>(FunctionCount - 1)),
            ExpectedSize);

  auto CallGraph =
      llvm::json::parse(takeString(neverd_callgraph_json(Session)));
  ASSERT_TRUE(static_cast<bool>(CallGraph));
  const llvm::json::Object *Root = CallGraph->getAsObject();
  ASSERT_NE(Root, nullptr);
  ASSERT_NE(Root->getArray("nodes"), nullptr);
  EXPECT_EQ(Root->getArray("nodes")->size(), FunctionCount);
  ASSERT_NE(Root->getArray("edges"), nullptr);
  EXPECT_TRUE(Root->getArray("edges")->empty());
}

TEST_F(SBFIntegrationTest,
       SemanticFunctionSizesOverrideELFSizesAndFailClosedAtTheWorkBudget) {
  constexpr size_t FunctionCount = 4'098;
  constexpr size_t FirstSharedSlot = FunctionCount;
  constexpr size_t SharedBlockCount = FunctionCount - 1;
  constexpr size_t LastSharedSlot = FirstSharedSlot + SharedBlockCount - 1;
  constexpr size_t AttackerClaimedSlotCount = 1;

  neverd::sbf::test::StrictELFOptions Options;
  Options.AddDebugSymbols = true;
  Options.Text.reserve((FunctionCount + SharedBlockCount) *
                       neverd::sbf::kInstructionSize);
  Options.FunctionSymbols.reserve(FunctionCount);
  for (size_t FunctionID = 0; FunctionID < FunctionCount; ++FunctionID) {
    const size_t TargetSlot =
        FunctionID < 2 ? FirstSharedSlot : FirstSharedSlot + FunctionID - 1;
    const int16_t Offset = static_cast<int16_t>(TargetSlot - FunctionID - 1);
    append(Options.Text,
           encode(neverd::sbf::Opcode::JA, /*Dst=*/0, /*Src=*/0, Offset));
    Options.FunctionSymbols.push_back({"function_" + std::to_string(FunctionID),
                                       FunctionID, AttackerClaimedSlotCount});
  }
  for (size_t Slot = FirstSharedSlot; Slot < LastSharedSlot; ++Slot)
    append(Options.Text, encode(neverd::sbf::Opcode::JA, /*Dst=*/0, /*Src=*/0,
                                /*Offset=*/0));
  append(Options.Text, encode(neverd::sbf::Opcode::EXIT));
  write(neverd::sbf::test::buildStrictELF(Options));

  ASSERT_EQ(neverd_session_load(Session, Path.string().c_str()), 1)
      << takeString(neverd_last_error(Session));
  ASSERT_EQ(neverd_session_analyze(Session), 1)
      << takeString(neverd_last_error(Session));
  ASSERT_EQ(neverd_func_count(Session), static_cast<int>(FunctionCount));

  const int ExactExpected =
      static_cast<int>((SharedBlockCount + 1) * neverd::sbf::kInstructionSize);
  EXPECT_EQ(neverd_func_size(Session, 0), ExactExpected)
      << "an exact semantic body must replace a nonzero ELF st_size";
  EXPECT_EQ(neverd_func_size(Session, static_cast<int>(FunctionCount - 1)), 0)
      << "a budget-exhausted body must not leak a nonzero ELF st_size";
}

TEST_F(SBFIntegrationTest,
       CallGraphOutputBudgetReturnsCanonicalEmptyGraphWithoutPartialEdges) {
  constexpr size_t CallerCount = 513;
  constexpr size_t TargetCount = 513;
  static_assert(CallerCount * TargetCount >
                neverd::sbf::kCallGraphOutputEdgeBudget);
  constexpr size_t SharedCallStart = CallerCount;
  constexpr size_t SharedExitSlot = SharedCallStart + TargetCount;
  constexpr size_t TargetStart = SharedExitSlot + 1;
  constexpr size_t SlotCount = TargetStart + TargetCount;

  neverd::sbf::test::StrictELFOptions Options;
  Options.AddDebugSymbols = true;
  Options.Text.reserve(SlotCount * neverd::sbf::kInstructionSize);
  Options.FunctionSymbols.reserve(CallerCount);
  for (size_t CallerID = 0; CallerID < CallerCount; ++CallerID) {
    append(Options.Text,
           encode(neverd::sbf::Opcode::JA, /*Dst=*/0, /*Src=*/0,
                  static_cast<int16_t>(SharedCallStart - CallerID - 1)));
    Options.FunctionSymbols.push_back(
        {"caller_" + std::to_string(CallerID), CallerID, /*SlotCount=*/0});
  }
  for (size_t TargetID = 0; TargetID < TargetCount; ++TargetID) {
    const size_t CallSlot = SharedCallStart + TargetID;
    const size_t TargetSlot = TargetStart + TargetID;
    append(Options.Text,
           encode(neverd::sbf::Opcode::CALL_IMM, /*Dst=*/0, /*Src=*/1,
                  /*Offset=*/0,
                  static_cast<int32_t>(TargetSlot - CallSlot - 1)));
  }
  append(Options.Text, encode(neverd::sbf::Opcode::EXIT));
  for (size_t TargetID = 0; TargetID < TargetCount; ++TargetID)
    append(Options.Text, encode(neverd::sbf::Opcode::EXIT));
  write(neverd::sbf::test::buildStrictELF(Options));

  ASSERT_EQ(neverd_session_load(Session, Path.string().c_str()), 1)
      << takeString(neverd_last_error(Session));
  ASSERT_EQ(neverd_session_analyze(Session), 1)
      << takeString(neverd_last_error(Session));

  auto CallGraph =
      llvm::json::parse(takeString(neverd_callgraph_json(Session)));
  ASSERT_TRUE(static_cast<bool>(CallGraph));
  const llvm::json::Object *Root = CallGraph->getAsObject();
  ASSERT_NE(Root, nullptr);
  const llvm::json::Array *Nodes = Root->getArray("nodes");
  const llvm::json::Array *Edges = Root->getArray("edges");
  ASSERT_NE(Nodes, nullptr);
  ASSERT_NE(Edges, nullptr);
  EXPECT_TRUE(Nodes->empty());
  EXPECT_TRUE(Edges->empty())
      << "budget failure must discard every otherwise exact edge";
  EXPECT_NE(takeString(neverd_last_error(Session))
                .find(neverd::sbf::kCallGraphEdgeBudgetDiagnostic.str()),
            std::string::npos);
}

TEST_F(SBFIntegrationTest,
       CallGraphByteBudgetCountsTheFinalEscapedJSONBeforeAllocation) {
  constexpr size_t TargetCount = 45'000;
  constexpr size_t MaxRetainedNameBytes =
      neverd::sbf::kELFDebugSymbolNameByteLimit - 1;
  constexpr size_t JSONUnicodeEscapeBytes = 6;
  static_assert(TargetCount < neverd::sbf::kCallGraphOutputEdgeBudget);
  static_assert(TargetCount < neverd::sbf::kCallGraphInputCallSiteBudget);
  static_assert(TargetCount * MaxRetainedNameBytes * JSONUnicodeEscapeBytes >
                neverd::sbf::kCallGraphOutputByteBudget);
  constexpr size_t SharedExitSlot = TargetCount;
  constexpr size_t TargetStart = SharedExitSlot + 1;
  constexpr size_t SlotCount = TargetStart + TargetCount;

  neverd::sbf::test::StrictELFOptions Options;
  Options.AddDebugSymbols = true;
  // U+0001 is valid UTF-8 but JSON must encode every byte as "\\u0001".
  // One maximum-size caller name therefore exercises the exact escaping pass
  // without requiring a near-edge-limit relation.
  Options.DebugSymbolName.assign(MaxRetainedNameBytes, '\x01');
  Options.Text.reserve(SlotCount * neverd::sbf::kInstructionSize);
  for (size_t TargetID = 0; TargetID < TargetCount; ++TargetID) {
    const size_t TargetSlot = TargetStart + TargetID;
    append(Options.Text,
           encode(neverd::sbf::Opcode::CALL_IMM, /*Dst=*/0, /*Src=*/1,
                  /*Offset=*/0,
                  static_cast<int32_t>(TargetSlot - TargetID - 1)));
  }
  append(Options.Text, encode(neverd::sbf::Opcode::EXIT));
  for (size_t TargetID = 0; TargetID < TargetCount; ++TargetID)
    append(Options.Text, encode(neverd::sbf::Opcode::EXIT));
  write(neverd::sbf::test::buildStrictELF(Options));

  ASSERT_EQ(neverd_session_load(Session, Path.string().c_str()), 1)
      << takeString(neverd_last_error(Session));
  ASSERT_EQ(neverd_session_analyze(Session), 1)
      << takeString(neverd_last_error(Session));

  auto CallGraph =
      llvm::json::parse(takeString(neverd_callgraph_json(Session)));
  ASSERT_TRUE(static_cast<bool>(CallGraph));
  const llvm::json::Object *Root = CallGraph->getAsObject();
  ASSERT_NE(Root, nullptr);
  ASSERT_NE(Root->getArray("nodes"), nullptr);
  ASSERT_NE(Root->getArray("edges"), nullptr);
  EXPECT_TRUE(Root->getArray("nodes")->empty());
  EXPECT_TRUE(Root->getArray("edges")->empty());
  EXPECT_NE(takeString(neverd_last_error(Session))
                .find(neverd::sbf::kCallGraphByteBudgetDiagnostic.str()),
            std::string::npos);
}

TEST_F(SBFIntegrationTest,
       StrictInvalidUTF8DebugNamesAreSafeAtEveryJSONBoundary) {
  std::string RawName = "strict_invalid_";
  RawName.push_back(static_cast<char>(0xff));
  const std::string MetadataName = neverd::escapeInvalidUTF8(RawName);

  neverd::sbf::test::StrictELFOptions Options;
  Options.AddDebugSymbols = true;
  Options.DebugSymbolName = RawName;
  write(neverd::sbf::test::buildStrictELF(Options));

  ASSERT_EQ(neverd_session_load(Session, Path.string().c_str()), 1)
      << takeString(neverd_last_error(Session));
  ASSERT_EQ(neverd_session_analyze(Session), 1)
      << takeString(neverd_last_error(Session));
  const neverd_va_t Entry = neverd_session_entry_addr(Session);

  auto Symbols = llvm::json::parse(takeString(neverd_symbols_json(Session)));
  ASSERT_TRUE(static_cast<bool>(Symbols));
  const llvm::json::Array *SymbolArray = Symbols->getAsArray();
  ASSERT_NE(SymbolArray, nullptr);
  ASSERT_EQ(SymbolArray->size(), 1u);
  ASSERT_NE((*SymbolArray)[0].getAsObject(), nullptr);
  EXPECT_EQ((*SymbolArray)[0].getAsObject()->getString("name"), MetadataName);

  const auto ExpectJSON = [&](const char *OwnedText) {
    const std::string Text = takeString(OwnedText);
    auto Parsed = llvm::json::parse(Text);
    if (!Parsed)
      ADD_FAILURE() << llvm::toString(Parsed.takeError()) << "\n" << Text;
  };
  ExpectJSON(neverd_entrypoints_json(Session));
  ExpectJSON(neverd_resolve_addr(Session, Entry));
  ExpectJSON(neverd_disasm_json(Session, Entry, /*MaxInsns=*/0));
  ExpectJSON(neverd_cfg_json(Session, Entry));
  ExpectJSON(neverd_callgraph_json(Session));
}

TEST_F(SBFIntegrationTest,
       LegacyInvalidUTF8DebugAndRelocationNamesRemainRawButJSONSafe) {
  std::string RawDebugName = "legacy_debug_";
  RawDebugName.push_back(static_cast<char>(0xff));
  std::string RawCallName = "legacy_call_";
  RawCallName.push_back(static_cast<char>(0xfe));
  const std::string MetadataDebugName = neverd::escapeInvalidUTF8(RawDebugName);
  const std::string MetadataCallName = neverd::escapeInvalidUTF8(RawCallName);
  const std::string SafeCallName = llvm::json::fixUTF8(RawCallName);
  const uint32_t RawCallKey = neverd::sbf::hashSymbolName(RawCallName);
  ASSERT_NE(RawCallKey, neverd::sbf::hashSymbolName(SafeCallName));

  neverd::sbf::test::LegacyDynamicELFOptions Options;
  Options.Text.reserve(2 * neverd::sbf::kInstructionSize);
  append(Options.Text, encode(neverd::sbf::Opcode::CALL_IMM));
  append(Options.Text, encode(neverd::sbf::Opcode::EXIT));
  Options.DynamicSymbolName = RawCallName;
  Options.StaticSymbolName = RawDebugName;
  Options.AddSameNamedStaticFunction = true;
  Options.Relocations.push_back(
      {0, neverd::sbf::Relocation::Call32,
       neverd::sbf::test::kLegacyFixtureDynamicSymbolIndex});
  write(neverd::sbf::test::buildLegacyDynamicELF(Options));

  auto LoadedImage = neverd::loadBinary(Path);
  ASSERT_TRUE(static_cast<bool>(LoadedImage))
      << llvm::toString(LoadedImage.takeError());
  ASSERT_TRUE(LoadedImage->SBF.has_value());
  ASSERT_EQ(LoadedImage->Relocations.size(), 1u);
  ASSERT_TRUE(LoadedImage->Relocations.front().ELF.has_value());
  ASSERT_TRUE(LoadedImage->Relocations.front().ELF->Symbol.has_value());
  ASSERT_TRUE(LoadedImage->Relocations.front().ELF->Symbol->Name.has_value());
  EXPECT_EQ(*LoadedImage->Relocations.front().ELF->Symbol->Name, RawCallName);
  auto ExecutableImage =
      neverd::sbf::buildProgramImage(*LoadedImage, *LoadedImage->SBF);
  ASSERT_TRUE(static_cast<bool>(ExecutableImage))
      << llvm::toString(ExecutableImage.takeError());
  ASSERT_GE(ExecutableImage->text().size(), neverd::sbf::kInstructionSize);
  EXPECT_EQ(llvm::support::endian::read32le(ExecutableImage->text().data() +
                                            neverd::sbf::kImmediateOffset),
            RawCallKey)
      << "presentation repair must not change relocation hashing";

  ASSERT_EQ(neverd_session_load(Session, Path.string().c_str()), 1)
      << takeString(neverd_last_error(Session));

  auto Symbols = llvm::json::parse(takeString(neverd_symbols_json(Session)));
  ASSERT_TRUE(static_cast<bool>(Symbols));
  const llvm::json::Array *SymbolArray = Symbols->getAsArray();
  ASSERT_NE(SymbolArray, nullptr);
  ASSERT_FALSE(SymbolArray->empty());
  ASSERT_NE(SymbolArray->front().getAsObject(), nullptr);
  EXPECT_EQ(SymbolArray->front().getAsObject()->getString("name"),
            MetadataDebugName);

  auto Relocations = llvm::json::parse(takeString(neverd_relocs_json(Session)));
  ASSERT_TRUE(static_cast<bool>(Relocations));
  const llvm::json::Array *RelocationArray = Relocations->getAsArray();
  ASSERT_NE(RelocationArray, nullptr);
  ASSERT_EQ(RelocationArray->size(), 1u);
  ASSERT_NE(RelocationArray->front().getAsObject(), nullptr);
  EXPECT_EQ(RelocationArray->front().getAsObject()->getString("symbol"),
            MetadataCallName);

  neverd_sbf_set_strict(Session, 0);
  ASSERT_EQ(neverd_session_analyze(Session), 1)
      << takeString(neverd_last_error(Session));
  const neverd_va_t Entry = neverd_session_entry_addr(Session);

  auto Disassembly = llvm::json::parse(
      takeString(neverd_disasm_json(Session, Entry, /*MaxInsns=*/0)));
  ASSERT_TRUE(static_cast<bool>(Disassembly));
  const llvm::json::Array *Instructions = Disassembly->getAsArray();
  ASSERT_NE(Instructions, nullptr);
  ASSERT_FALSE(Instructions->empty());
  ASSERT_NE(Instructions->front().getAsObject(), nullptr);
  const std::optional<llvm::StringRef> Operand =
      Instructions->front().getAsObject()->getString("op_str");
  ASSERT_TRUE(Operand.has_value());
  EXPECT_NE(Operand->find(SafeCallName), llvm::StringRef::npos);

  const auto ExpectJSON = [&](const char *OwnedText) {
    const std::string Text = takeString(OwnedText);
    auto Parsed = llvm::json::parse(Text);
    if (!Parsed)
      ADD_FAILURE() << llvm::toString(Parsed.takeError()) << "\n" << Text;
  };
  ExpectJSON(neverd_entrypoints_json(Session));
  ExpectJSON(neverd_resolve_addr(Session, Entry));
  ExpectJSON(neverd_cfg_json(Session, Entry));
  ExpectJSON(neverd_callgraph_json(Session));
}

} // namespace
