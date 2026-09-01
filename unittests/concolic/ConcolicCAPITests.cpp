//===- ConcolicCAPITests.cpp - Public LowIR concolic JSON ABI ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lib/sdk/capi/SessionImpl.h"
#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPI.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/SHA256.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace {

std::string takeOwned(const char *Text) {
  std::string Result = Text ? Text : "";
  neverd_free_string(Text);
  return Result;
}

llvm::json::Object parseObject(const char *Text) {
  const std::string Owned = takeOwned(Text);
  llvm::Expected<llvm::json::Value> Parsed = llvm::json::parse(Owned);
  if (!Parsed) {
    ADD_FAILURE() << "invalid JSON report";
    llvm::consumeError(Parsed.takeError());
    return {};
  }
  const llvm::json::Object *Object = Parsed->getAsObject();
  if (!Object) {
    ADD_FAILURE() << "concolic report is not an object";
    return {};
  }
  return *Object;
}

class OwnedSession {
public:
  OwnedSession() : Value(neverd_session_create()) {}
  ~OwnedSession() { neverd_session_destroy(Value); }

  OwnedSession(const OwnedSession &) = delete;
  OwnedSession &operator=(const OwnedSession &) = delete;

  operator neverd_session_t() const { return Value; }

private:
  neverd_session_t Value;
};

std::string sha256(llvm::ArrayRef<uint8_t> Bytes) {
  llvm::SHA256 Hash;
  Hash.update(Bytes);
  const auto Digest = Hash.final();
  static constexpr char Digits[] = "0123456789abcdef";
  std::string Result;
  Result.reserve(Digest.size() * 2);
  for (uint8_t Byte : Digest) {
    Result.push_back(Digits[Byte >> 4]);
    Result.push_back(Digits[Byte & 0xf]);
  }
  return Result;
}

std::vector<uint8_t> readFile(const std::filesystem::path &Path) {
  std::ifstream Input(Path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(Input),
                              std::istreambuf_iterator<char>());
}

class ScopedTempDir {
public:
  ScopedTempDir() {
    const auto Stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path Parent = std::filesystem::temp_directory_path();
    for (unsigned Attempt = 0; Attempt != 100; ++Attempt) {
      const std::filesystem::path Candidate =
          Parent / ("neverd-concolic-capi-" + std::to_string(Stamp) + "-" +
                    std::to_string(reinterpret_cast<uintptr_t>(this)) + "-" +
                    std::to_string(Attempt));
      std::error_code Error;
      if (std::filesystem::create_directory(Candidate, Error)) {
        Path = Candidate;
        return;
      }
      if (Error && Error != std::errc::file_exists) {
        ADD_FAILURE() << Error.message();
        return;
      }
    }
    ADD_FAILURE() << "could not create a unique concolic test directory";
  }

  ~ScopedTempDir() {
    if (Path.empty())
      return;
    std::error_code Error;
    std::filesystem::remove_all(Path, Error);
  }

  const std::filesystem::path &path() const { return Path; }

private:
  std::filesystem::path Path;
};

llvm::json::Object
callUnloaded(const neverd_lowir_concolic_options_v1 *Options) {
  OwnedSession Session;
  EXPECT_NE(static_cast<neverd_session_t>(Session), nullptr);
  return parseObject(neverd_lowir_concolic_json_v1(Session, 0x401000, Options));
}

void expectInvalidOptions(const neverd_lowir_concolic_options_v1 &Options) {
  llvm::json::Object Report = callUnloaded(&Options);
  EXPECT_EQ(Report.getBoolean("ok"), false);
  EXPECT_EQ(Report.getBoolean("exhaustive"), false);
  EXPECT_EQ(Report.getString("error_code"), "invalid_options");
  EXPECT_TRUE(Report.getString("error").has_value());
}

TEST(NeverDConcolicCAPI, InvalidSessionReturnsAnOwnedVersionedErrorReport) {
  llvm::json::Object Report =
      parseObject(neverd_lowir_concolic_json_v1(nullptr, 0x401000, nullptr));

  EXPECT_EQ(Report.getInteger("schema_version"), 1);
  EXPECT_EQ(Report.getString("adapter"), "lowir-concolic-v1");
  EXPECT_EQ(Report.getString("mode"), "concolic");
  EXPECT_EQ(Report.getBoolean("ok"), false);
  EXPECT_EQ(Report.getBoolean("exhaustive"), false);
  EXPECT_EQ(Report.getString("error_code"), "invalid_session");
  EXPECT_TRUE(Report.getString("error").has_value());
}

TEST(NeverDConcolicCAPI, UnloadedSessionFailsBeforeAnalysis) {
  neverd_session_t Session = neverd_session_create();
  ASSERT_NE(Session, nullptr);

  llvm::json::Object Report =
      parseObject(neverd_lowir_concolic_json_v1(Session, 0x401000, nullptr));
  EXPECT_EQ(Report.getBoolean("ok"), false);
  EXPECT_EQ(Report.getBoolean("exhaustive"), false);
  EXPECT_EQ(Report.getString("error_code"), "no_binary_loaded");

  neverd_session_destroy(Session);
}

TEST(NeverDConcolicCAPI, NativeExceptionsNeverCrossTheCABI) {
  OwnedSession Session;
  ASSERT_NE(static_cast<neverd_session_t>(Session), nullptr);
  auto *Internal =
      neverd::sdk::toSession(static_cast<neverd_session_t>(Session));

  const auto ExpectInternalError = [&](llvm::StringRef ExpectedMessage) {
    llvm::json::Object Report =
        parseObject(neverd_lowir_concolic_json_v1(Session, 0x401000, nullptr));
    EXPECT_EQ(Report.getInteger("schema_version"), 1);
    EXPECT_EQ(Report.getString("adapter"), "lowir-concolic-v1");
    EXPECT_EQ(Report.getString("mode"), "concolic");
    EXPECT_EQ(Report.getBoolean("ok"), false);
    EXPECT_EQ(Report.getBoolean("exhaustive"), false);
    EXPECT_EQ(Report.getString("error_code"), "internal_error");
    EXPECT_NE(Report.getString("error")
                  .value_or(llvm::StringRef())
                  .find(ExpectedMessage),
              llvm::StringRef::npos);
  };

  Internal->LowIRConcolicBeforeRunForTesting = [] {
    throw std::runtime_error("injected standard concolic exception");
  };
  ExpectInternalError("injected standard concolic exception");

  Internal->LowIRConcolicBeforeRunForTesting = [] { throw 7; };
  ExpectInternalError("non-standard native exception");

  Internal->LowIRConcolicBeforeRunForTesting = [] { throw std::bad_alloc(); };
  const char *AllocationFailure =
      neverd_lowir_concolic_json_v1(Session, 0x401000, nullptr);
  EXPECT_EQ(AllocationFailure, nullptr);
  neverd_free_string(AllocationFailure);
  EXPECT_NE(takeOwned(neverd_last_error(Session)).find("allocation failed"),
            std::string::npos);
}

TEST(NeverDConcolicCAPI, RejectsATornSeedPointerCountPrefix) {
  struct ShortOptions {
    size_t struct_size;
    const neverd_lowir_concolic_register_seed_v1 *register_seeds;
    uint32_t canary;
  } Options{offsetof(ShortOptions, canary), nullptr, 0x636f6e63u};

  neverd_session_t Session = neverd_session_create();
  ASSERT_NE(Session, nullptr);
  llvm::json::Object Report = parseObject(neverd_lowir_concolic_json_v1(
      Session, 0x401000,
      reinterpret_cast<const neverd_lowir_concolic_options_v1 *>(&Options)));

  EXPECT_EQ(Report.getBoolean("ok"), false);
  EXPECT_EQ(Report.getString("error_code"), "invalid_options");
  EXPECT_EQ(Options.canary, 0x636f6e63u);
  neverd_session_destroy(Session);
}

TEST(NeverDConcolicCAPI, ReadsOnlyCompleteAppendOnlyPrefixes) {
#define FIELD_END(Field)                                                       \
  (offsetof(neverd_lowir_concolic_options_v1, Field) +                         \
   sizeof(static_cast<neverd_lowir_concolic_options_v1 *>(nullptr)->Field))
  const std::vector<size_t> Accepted{
      FIELD_END(struct_size),
      FIELD_END(register_seed_count),
      FIELD_END(max_steps),
      FIELD_END(max_block_visits),
      FIELD_END(max_loop_iterations),
      FIELD_END(max_flip_attempts),
      FIELD_END(max_candidates),
      FIELD_END(reserved),
      FIELD_END(solver_max_conflicts),
      FIELD_END(solver_max_propagations),
      FIELD_END(solver_max_watch_visits),
      FIELD_END(solver_max_gates),
  };

  for (size_t Size : Accepted) {
    SCOPED_TRACE(Size);
    neverd_lowir_concolic_options_v1 Options{};
    Options.struct_size = Size;
    llvm::json::Object Report = callUnloaded(&Options);
    EXPECT_EQ(Report.getBoolean("ok"), false);
    EXPECT_EQ(Report.getString("error_code"), "no_binary_loaded");
  }

  for (size_t Size = 0; Size < sizeof(neverd_lowir_concolic_options_v1);
       ++Size) {
    if (std::find(Accepted.begin(), Accepted.end(), Size) != Accepted.end())
      continue;
    SCOPED_TRACE(Size);
    neverd_lowir_concolic_options_v1 Options{};
    Options.struct_size = Size;
    expectInvalidOptions(Options);
  }
#undef FIELD_END
}

TEST(NeverDConcolicCAPI, AcceptsAFutureAppendOnlySuffix) {
  struct FutureOptions {
    neverd_lowir_concolic_options_v1 v1{};
    uint64_t future = 0x6675747572657632ULL;
  } Options;
  Options.v1.struct_size = sizeof(Options);

  llvm::json::Object Report = callUnloaded(&Options.v1);
  EXPECT_EQ(Report.getBoolean("ok"), false);
  EXPECT_EQ(Report.getString("error_code"), "no_binary_loaded");
  EXPECT_EQ(Options.future, 0x6675747572657632ULL);
}

TEST(NeverDConcolicCAPI, RejectsInvalidSeedArraysBeforeDereferencingThem) {
  neverd_lowir_concolic_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  Options.register_seed_count = 1;
  expectInvalidOptions(Options);

  Options.register_seed_count =
      NEVERD_LOWIR_CONCOLIC_MAX_REGISTER_SEEDS_V1 + size_t{1};
  expectInvalidOptions(Options);
}

TEST(NeverDConcolicCAPI, IgnoresANonnullSeedPointerWhenCountIsZero) {
  const neverd_lowir_concolic_register_seed_v1 Poison{
      std::numeric_limits<uint64_t>::max(),
      std::numeric_limits<uint64_t>::max(), 0, 1};
  neverd_lowir_concolic_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  Options.register_seeds = &Poison;

  llvm::json::Object Report = callUnloaded(&Options);
  EXPECT_EQ(Report.getBoolean("ok"), false);
  EXPECT_EQ(Report.getString("error_code"), "no_binary_loaded");
}

TEST(NeverDConcolicCAPI, RejectsInvalidSeedWidthsValuesAndRanges) {
  neverd_lowir_concolic_register_seed_v1 Seed{0, 0, 0, 0};
  neverd_lowir_concolic_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  Options.register_seeds = &Seed;
  Options.register_seed_count = 1;

  expectInvalidOptions(Options);
  Seed.bytes = 9;
  expectInvalidOptions(Options);
  Seed.bytes = 1;
  Seed.value = 0x100;
  expectInvalidOptions(Options);
  Seed.bytes = 8;
  Seed.value = 0;
  Seed.offset = std::numeric_limits<uint64_t>::max() - 7;
  expectInvalidOptions(Options);
  Seed.offset = 0;
  Seed.reserved = 1;
  expectInvalidOptions(Options);
}

TEST(NeverDConcolicCAPI, RejectsOverlappingSeedRangesWithoutMutatingInput) {
  neverd_lowir_concolic_register_seed_v1 Seeds[] = {
      {10, 0xaabb, 2, 0},
      {9, 0xccdd, 2, 0},
  };
  const neverd_lowir_concolic_register_seed_v1 Before[] = {Seeds[0], Seeds[1]};
  neverd_lowir_concolic_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  Options.register_seeds = Seeds;
  Options.register_seed_count = 2;

  expectInvalidOptions(Options);
  EXPECT_EQ(Seeds[0].offset, Before[0].offset);
  EXPECT_EQ(Seeds[0].value, Before[0].value);
  EXPECT_EQ(Seeds[1].offset, Before[1].offset);
  EXPECT_EQ(Seeds[1].value, Before[1].value);
}

TEST(NeverDConcolicCAPI, RejectsNonzeroOptionsReservedField) {
  neverd_lowir_concolic_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  Options.reserved = 1;
  expectInvalidOptions(Options);
}

TEST(NeverDConcolicCAPI, ReportsFunctionNotFoundAsATypedSessionError) {
  const std::filesystem::path Fixture =
      std::filesystem::path(NEVERD_CONCOLIC_FIXTURE_DIR) /
      "lowir_concolic_elf_x64";
  OwnedSession Session;
  ASSERT_NE(static_cast<neverd_session_t>(Session), nullptr);
  ASSERT_EQ(neverd_session_load(Session, Fixture.string().c_str()), 1)
      << takeOwned(neverd_last_error(Session));

  llvm::json::Object Report = parseObject(neverd_lowir_concolic_json_v1(
      Session, std::numeric_limits<neverd_va_t>::max(), nullptr));
  EXPECT_EQ(Report.getBoolean("ok"), false);
  EXPECT_EQ(Report.getBoolean("exhaustive"), false);
  EXPECT_EQ(Report.getString("error_code"), "function_not_found");
  EXPECT_FALSE(takeOwned(neverd_last_error(Session)).empty());
}

TEST(NeverDConcolicCAPI, RejectsEVMBeforeRunningNativeLowIRAnalysis) {
  ScopedTempDir Temp;
  ASSERT_FALSE(Temp.path().empty());
  const std::filesystem::path Input = Temp.path() / "minimal.evm";
  {
    std::ofstream Bytecode(Input, std::ios::binary);
    ASSERT_TRUE(Bytecode.is_open());
    Bytecode << "60006000f3";
  }

  OwnedSession Session;
  ASSERT_NE(static_cast<neverd_session_t>(Session), nullptr);
  ASSERT_EQ(neverd_session_load(Session, Input.string().c_str()), 1)
      << takeOwned(neverd_last_error(Session));
  llvm::json::Object Report =
      parseObject(neverd_lowir_concolic_json_v1(Session, 0, nullptr));
  EXPECT_EQ(Report.getBoolean("ok"), false);
  EXPECT_EQ(Report.getString("error_code"), "unsupported_target");
  EXPECT_FALSE(takeOwned(neverd_last_error(Session)).empty());
}

TEST(NeverDConcolicCAPI,
     PublishesDeterministicVerifiedJSONBoundToTheLoadedSnapshot) {
  const std::filesystem::path Fixture =
      std::filesystem::path(NEVERD_CONCOLIC_FIXTURE_DIR) /
      "lowir_concolic_elf_x64";
  const std::vector<uint8_t> FixtureBytes = readFile(Fixture);
  ASSERT_FALSE(FixtureBytes.empty());
  const std::string ExpectedSHA256 = sha256(FixtureBytes);

  ScopedTempDir Temp;
  ASSERT_FALSE(Temp.path().empty());
  const std::filesystem::path Input = Temp.path() / "loaded-snapshot";
  std::error_code Error;
  ASSERT_TRUE(std::filesystem::copy_file(Fixture, Input, Error))
      << Error.message();

  OwnedSession Session;
  ASSERT_NE(static_cast<neverd_session_t>(Session), nullptr);
  ASSERT_EQ(neverd_session_load(Session, Input.string().c_str()), 1)
      << takeOwned(neverd_last_error(Session));

  // The public report must identify the bytes the session actually loaded,
  // even if the path now names different bytes.
  {
    std::ofstream Mutated(Input, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(Mutated.is_open());
    Mutated << "different bytes after session load";
  }

  int FunctionIndex = neverd_func_find_by_name(Session, "concolic_branch");
  ASSERT_GE(FunctionIndex, 0) << takeOwned(neverd_last_error(Session));
  const neverd_va_t Entry = neverd_func_entry(Session, FunctionIndex);
  ASSERT_NE(Entry, 0u);

  const neverd_lowir_concolic_register_seed_v1 Seed{56, 0, 4, 0};
  neverd_lowir_concolic_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  Options.register_seeds = &Seed;
  Options.register_seed_count = 1;

  neverd_lowir_concolic_options_v1 Invalid = Options;
  Invalid.reserved = 1;
  EXPECT_EQ(parseObject(neverd_lowir_concolic_json_v1(Session, Entry, &Invalid))
                .getString("error_code"),
            "invalid_options");

  const std::string First =
      takeOwned(neverd_lowir_concolic_json_v1(Session, Entry, &Options));
  const std::string Second =
      takeOwned(neverd_lowir_concolic_json_v1(Session, Entry, &Options));
  EXPECT_EQ(First, Second);

  llvm::json::Object Report =
      parseObject(neverd_lowir_concolic_json_v1(Session, Entry, &Options));
  ASSERT_EQ(Report.getInteger("schema_version"), 1);
  EXPECT_EQ(Report.getString("adapter"), "lowir-concolic-v1");
  EXPECT_EQ(Report.getString("mode"), "concolic");
  EXPECT_EQ(Report.getBoolean("ok"), true);
  EXPECT_EQ(Report.getBoolean("exhaustive"), false);
  EXPECT_EQ(Report.getBoolean("trace_complete"), true);
  EXPECT_EQ(Report.getBoolean("trace_exact"), true);
  EXPECT_EQ(Report.getString("trace_reason"), "none");
  EXPECT_EQ(Report.getString("trace_outcome"), "returned");
  EXPECT_GT(Report.getInteger("executed_steps").value_or(0), 0);
  EXPECT_EQ(Report.getInteger("unmodelled_ops"), 0);
  EXPECT_EQ(Report.getInteger("opaque_ops"), 0);
  EXPECT_EQ(Report.getInteger("call_havocs"), 0);
  EXPECT_EQ(Report.getInteger("memory_havocs"), 0);
  EXPECT_EQ(Report.getInteger("flip_attempts"), 1);
  EXPECT_EQ(Report.getBoolean("flip_budget_hit"), false);
  EXPECT_EQ(Report.getBoolean("candidate_budget_hit"), false);

  const llvm::json::Object *Image = Report.getObject("image");
  ASSERT_NE(Image, nullptr);
  EXPECT_EQ(Image->getString("identity_status"), "exact_loaded_snapshot");
  EXPECT_EQ(Image->getString("sha256"), ExpectedSHA256);
  EXPECT_EQ(Image->getString("format"), "ELF");
  EXPECT_EQ(Image->getString("arch"), "x86_64");
  EXPECT_EQ(Image->getInteger("bits"), 64);
  EXPECT_EQ(Image->getString("endianness"), "little");

  const llvm::json::Object *Function = Report.getObject("function");
  ASSERT_NE(Function, nullptr);
  EXPECT_EQ(Function->getString("name"), "concolic_branch");
  EXPECT_EQ(Function->getBoolean("lift_complete"), true);

  const llvm::json::Object *Limits = Report.getObject("limits");
  ASSERT_NE(Limits, nullptr);
  EXPECT_EQ(Limits->size(), 10u);
  EXPECT_EQ(Limits->getInteger("max_steps"), 1 << 16);
  EXPECT_EQ(Limits->getInteger("max_block_visits"), 3);
  EXPECT_EQ(Limits->getInteger("max_loop_iterations"), 3);
  EXPECT_EQ(Limits->getInteger("max_flip_attempts"), 64);
  EXPECT_EQ(Limits->getInteger("max_candidates"), 64);
  EXPECT_EQ(Limits->getInteger("solver_max_conflicts"), 1 << 18);
  EXPECT_EQ(Limits->getInteger("solver_max_propagations"), 1 << 24);
  EXPECT_EQ(Limits->getInteger("solver_max_watch_visits"), 1 << 26);
  EXPECT_EQ(Limits->getInteger("solver_max_width"), 256);
  EXPECT_EQ(Limits->getInteger("solver_max_gates"), 1 << 22);

  const llvm::json::Array *InitialSeed = Report.getArray("initial_seed");
  ASSERT_NE(InitialSeed, nullptr);
  ASSERT_EQ(InitialSeed->size(), 1u);
  const llvm::json::Object *Initial = (*InitialSeed)[0].getAsObject();
  ASSERT_NE(Initial, nullptr);
  EXPECT_EQ(Initial->getString("offset"), "0x0000000000000038");
  EXPECT_EQ(Initial->getInteger("bytes"), 4);
  EXPECT_EQ(Initial->getString("value"), "0x00000000");

  const llvm::json::Array *Blocks = Report.getArray("blocks");
  ASSERT_NE(Blocks, nullptr);
  EXPECT_FALSE(Blocks->empty());
  const llvm::json::Array *Decisions = Report.getArray("decisions");
  ASSERT_NE(Decisions, nullptr);
  ASSERT_EQ(Decisions->size(), 1u);
  const llvm::json::Object *Decision = (*Decisions)[0].getAsObject();
  ASSERT_NE(Decision, nullptr);
  EXPECT_EQ(Decision->getInteger("decision_id"), 0);
  EXPECT_EQ(Decision->getBoolean("taken"), true);
  EXPECT_EQ(Decision->getInteger("constraint_prefix"), 0);
  EXPECT_EQ(Decision->getBoolean("concrete"), true);
  const llvm::json::Object *Occurrence = Decision->getObject("occurrence");
  ASSERT_NE(Occurrence, nullptr);
  EXPECT_EQ(Occurrence->getString("va"), "0x0000000000001243");
  EXPECT_EQ(Occurrence->getInteger("seq"), 1);
  EXPECT_EQ(Occurrence->getInteger("block_id"), 0);
  EXPECT_EQ(Occurrence->getInteger("op_index"), 14);
  EXPECT_EQ(Occurrence->getInteger("invocation"), 0);
  EXPECT_EQ(Occurrence->getString("kind"), "conditional_branch");
  const llvm::json::Array *Flips = Report.getArray("flips");
  ASSERT_NE(Flips, nullptr);
  ASSERT_EQ(Flips->size(), 1u);
  const llvm::json::Object *Flip = (*Flips)[0].getAsObject();
  ASSERT_NE(Flip, nullptr);
  EXPECT_EQ(Flip->getInteger("decision_id"), 0);
  EXPECT_EQ(Flip->getInteger("candidate_id"), 0);
  EXPECT_EQ(Flip->getBoolean("original_taken"), true);
  EXPECT_EQ(Flip->getInteger("constraint_prefix"), 0);
  EXPECT_EQ(Flip->getString("status"), "verified");
  EXPECT_EQ(Flip->getString("solver_status"), "sat");
  EXPECT_EQ(Flip->getString("encoding_error"), "none");
  EXPECT_EQ(Flip->getString("projection_status"), "accepted");
  EXPECT_EQ(Flip->getString("projection_reason"), "none");
  EXPECT_EQ(Flip->getString("replay_status"), "verified");
  EXPECT_EQ(Flip->getString("replay_reason"), "none");

  const llvm::json::Array *Candidates = Report.getArray("candidates");
  ASSERT_NE(Candidates, nullptr);
  ASSERT_EQ(Candidates->size(), 1u);
  const llvm::json::Object *Candidate = (*Candidates)[0].getAsObject();
  ASSERT_NE(Candidate, nullptr);
  EXPECT_EQ(Candidate->getInteger("candidate_id"), 0);
  const llvm::json::Array *CandidateSeed = Candidate->getArray("seed");
  ASSERT_NE(CandidateSeed, nullptr);
  ASSERT_EQ(CandidateSeed->size(), 1u);
  const llvm::json::Object *CandidateRange = (*CandidateSeed)[0].getAsObject();
  ASSERT_NE(CandidateRange, nullptr);
  EXPECT_EQ(CandidateRange->getString("offset"), "0x0000000000000038");
  EXPECT_EQ(CandidateRange->getInteger("bytes"), 4);
  EXPECT_EQ(CandidateRange->getString("value"), "0x00000007");
  EXPECT_TRUE(takeOwned(neverd_last_error(Session)).empty());
}

} // namespace
