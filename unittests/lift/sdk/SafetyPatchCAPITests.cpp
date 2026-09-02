//===- SafetyPatchCAPITests.cpp - Transactional sanitizer C API tests ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "SessionImpl.h"
#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPI.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Object/Binary.h"
#include "llvm/Object/MachO.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Program.h"

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef NEVERD_SANITIZE_FIXTURE_ROOT
#define NEVERD_SANITIZE_FIXTURE_ROOT ""
#endif
#ifndef NEVERD_SAFETY_FIXTURE_ROOT
#define NEVERD_SAFETY_FIXTURE_ROOT ""
#endif
#ifndef NEVERD_SANITIZE_RUNTIME_SOURCE
#define NEVERD_SANITIZE_RUNTIME_SOURCE ""
#endif
#ifndef NEVERD_SANITIZE_EMPTY_SOURCE
#define NEVERD_SANITIZE_EMPTY_SOURCE ""
#endif
#ifndef NEVERD_RUNTIME_FIXTURE_COMPILER
#define NEVERD_RUNTIME_FIXTURE_COMPILER ""
#endif

namespace {

bool hasRuntimeFixtureCompiler() {
  return !llvm::StringRef(NEVERD_RUNTIME_FIXTURE_COMPILER).empty();
}

#define REQUIRE_RUNTIME_FIXTURE_COMPILER()                                     \
  do {                                                                         \
    if (!hasRuntimeFixtureCompiler())                                          \
      GTEST_SKIP() << "runtime sanitizer fixture tests require a GNU/Clang "   \
                      "host C compiler";                                       \
  } while (false)

static_assert(offsetof(neverd_sanitize_options_v1, struct_size) == 0);
static_assert(offsetof(neverd_sanitize_options_v1, strategy) == sizeof(size_t));
static_assert(offsetof(neverd_sanitize_result_v1, struct_size) == 0);
static_assert(sizeof(neverd_sanitize_strategy_t) == sizeof(uint32_t));
static_assert(sizeof(neverd_sanitize_status_t) == sizeof(uint32_t));
static_assert(sizeof(neverd_sanitize_result_v1::status) == sizeof(uint32_t));
static_assert(offsetof(neverd_sanitize_result_v1, publication_outcome) == 80);
static_assert(offsetof(neverd_sanitize_result_v1,
                       publication_receipt_version) == 84);
static_assert(offsetof(neverd_sanitize_result_v1,
                       publication_operand_binding) == 100);
static_assert(sizeof(neverd_sanitize_result_v1) == 104);

struct SessionGuard {
  neverd_session_t Value = neverd_session_create();
  ~SessionGuard() { neverd_session_destroy(Value); }
};

std::string takeString(const char *Owned) {
  std::string Result = Owned ? Owned : "";
  neverd_free_string(Owned);
  return Result;
}

std::vector<uint8_t> readBytes(const std::filesystem::path &Path) {
  std::ifstream Stream(Path, std::ios::binary);
  return {std::istreambuf_iterator<char>(Stream),
          std::istreambuf_iterator<char>()};
}

void writeBytes(const std::filesystem::path &Path,
                const std::vector<uint8_t> &Bytes) {
  std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
  Stream.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
}

void writeBE32(std::vector<uint8_t> &Bytes, size_t Offset, uint32_t Value) {
  ASSERT_LE(Offset + sizeof(Value), Bytes.size());
  llvm::support::endian::write32be(Bytes.data() + Offset, Value);
}

std::vector<uint8_t> makeUniversalMachO(std::vector<uint8_t> X64,
                                        std::vector<uint8_t> ARM64) {
  using namespace llvm::MachO;
  constexpr size_t Alignment = 0x1000;
  const size_t TableSize = sizeof(fat_header) + 2 * sizeof(fat_arch);
  const size_t X64Offset = (TableSize + Alignment - 1) & ~(Alignment - 1);
  const size_t ARM64Offset =
      (X64Offset + X64.size() + Alignment - 1) & ~(Alignment - 1);
  std::vector<uint8_t> Bytes(ARM64Offset + ARM64.size(), 0);
  writeBE32(Bytes, offsetof(fat_header, magic), FAT_MAGIC);
  writeBE32(Bytes, offsetof(fat_header, nfat_arch), 2);
  const size_t X64Arch = sizeof(fat_header);
  writeBE32(Bytes, X64Arch + offsetof(fat_arch, cputype), CPU_TYPE_X86_64);
  writeBE32(Bytes, X64Arch + offsetof(fat_arch, cpusubtype),
            CPU_SUBTYPE_MULTIPLE);
  writeBE32(Bytes, X64Arch + offsetof(fat_arch, offset), X64Offset);
  writeBE32(Bytes, X64Arch + offsetof(fat_arch, size), X64.size());
  writeBE32(Bytes, X64Arch + offsetof(fat_arch, align), 12);
  const size_t ARM64Arch = X64Arch + sizeof(fat_arch);
  writeBE32(Bytes, ARM64Arch + offsetof(fat_arch, cputype), CPU_TYPE_ARM64);
  writeBE32(Bytes, ARM64Arch + offsetof(fat_arch, cpusubtype),
            CPU_SUBTYPE_MULTIPLE);
  writeBE32(Bytes, ARM64Arch + offsetof(fat_arch, offset), ARM64Offset);
  writeBE32(Bytes, ARM64Arch + offsetof(fat_arch, size), ARM64.size());
  writeBE32(Bytes, ARM64Arch + offsetof(fat_arch, align), 12);
  std::copy(X64.begin(), X64.end(), Bytes.begin() + X64Offset);
  std::copy(ARM64.begin(), ARM64.end(), Bytes.begin() + ARM64Offset);
  return Bytes;
}

bool corruptPrimaryCodeHash(std::vector<uint8_t> &Bytes) {
  using namespace llvm::MachO;
  const llvm::StringRef Contents(reinterpret_cast<const char *>(Bytes.data()),
                                 Bytes.size());
  llvm::Expected<std::unique_ptr<llvm::object::Binary>> Parsed =
      llvm::object::createBinary(
          llvm::MemoryBufferRef(Contents, "<signed-fixture>"));
  if (!Parsed) {
    llvm::consumeError(Parsed.takeError());
    return false;
  }
  const auto *Object =
      llvm::dyn_cast<llvm::object::MachOObjectFile>(Parsed->get());
  if (!Object)
    return false;
  for (const auto &Command : Object->load_commands()) {
    if (Command.C.cmd != LC_CODE_SIGNATURE)
      continue;
    const linkedit_data_command Signature =
        Object->getLinkeditDataLoadCommand(Command);
    if (uint64_t(Signature.dataoff) + Signature.datasize > Bytes.size() ||
        Signature.datasize < sizeof(CS_SuperBlob))
      return false;
    const uint8_t *Super = Bytes.data() + Signature.dataoff;
    const uint32_t Count =
        llvm::support::endian::read32be(Super + offsetof(CS_SuperBlob, count));
    if (sizeof(CS_SuperBlob) + uint64_t(Count) * sizeof(CS_BlobIndex) >
        Signature.datasize)
      return false;
    for (uint32_t Index = 0; Index < Count; ++Index) {
      const uint8_t *Entry =
          Super + sizeof(CS_SuperBlob) + Index * sizeof(CS_BlobIndex);
      const uint32_t Type =
          llvm::support::endian::read32be(Entry + offsetof(CS_BlobIndex, type));
      if (Type != CSSLOT_CODEDIRECTORY)
        continue;
      const uint32_t DirectoryOffset = llvm::support::endian::read32be(
          Entry + offsetof(CS_BlobIndex, offset));
      if (DirectoryOffset + sizeof(CS_CodeDirectory) > Signature.datasize)
        return false;
      const uint8_t *Directory = Super + DirectoryOffset;
      const uint32_t HashOffset = llvm::support::endian::read32be(
          Directory + offsetof(CS_CodeDirectory, hashOffset));
      const uint32_t CodeSlots = llvm::support::endian::read32be(
          Directory + offsetof(CS_CodeDirectory, nCodeSlots));
      const uint64_t HashByte =
          uint64_t(Signature.dataoff) + DirectoryOffset + HashOffset;
      if (CodeSlots == 0 || HashByte >= Bytes.size())
        return false;
      Bytes[HashByte] ^= 0x5a;
      return true;
    }
  }
  return false;
}

int runProgram(llvm::ArrayRef<llvm::StringRef> Arguments, std::string &Error) {
  if (Arguments.empty()) {
    Error = "program arguments are empty";
    return -1;
  }
  return llvm::sys::ExecuteAndWait(Arguments.front(), Arguments, std::nullopt,
                                   {}, 0, 0, &Error);
}

void seedPriorSessionResults(neverd::sdk::Session &Session) {
  Session.LastPatch.Success = true;
  Session.LastPatch.OutputPath = "prior-output";
  Session.LastPatch.CodeSize = 101;
  Session.LastPatch.TrampolineCount = 7;
  Session.LastSubstitutionCount = 1;
  Session.LastConstEncCount = 2;
  Session.LastOpaquePredCount = 3;
  Session.LastFlattenCount = 4;
  Session.LastBogusCount = 5;
  Session.LastIndirectBranchCount = 6;
  Session.LastIndirectCallCount = 7;
  Session.LastMBACount = 8;
  Session.LastIndirectGlobalCount = 9;
  Session.LastValueLaunderCount = 10;
  Session.LastConstPoolCount = 11;
  Session.LastBitMaskCount = 12;
}

void expectPriorSessionResults(neverd_session_t Session) {
  EXPECT_EQ(takeString(neverd_patch_output_path(Session)), "prior-output");
  EXPECT_EQ(neverd_patch_code_size(Session), 101u);
  EXPECT_EQ(neverd_patch_trampoline_count(Session), 7);
  EXPECT_EQ(neverd_patch_substitution_count(Session), 1);
  EXPECT_EQ(neverd_patch_constant_encryption_count(Session), 2);
  EXPECT_EQ(neverd_patch_opaque_predicate_count(Session), 3);
  EXPECT_EQ(neverd_patch_control_flow_flattening_count(Session), 4);
  EXPECT_EQ(neverd_patch_bogus_control_flow_count(Session), 5);
  EXPECT_EQ(neverd_patch_indirect_branch_count(Session), 6);
  EXPECT_EQ(neverd_patch_indirect_call_count(Session), 7);
  EXPECT_EQ(neverd_patch_mba_count(Session), 8);
  EXPECT_EQ(neverd_patch_indirect_global_count(Session), 9);
  EXPECT_EQ(neverd_patch_value_launder_count(Session), 10);
  EXPECT_EQ(neverd_patch_constant_pooling_count(Session), 11);
  EXPECT_EQ(neverd_patch_bit_masking_count(Session), 12);
}

class TemporaryFixture {
public:
  explicit TemporaryFixture(std::filesystem::path Source = {}) {
    llvm::SmallString<128> Unique;
    if (std::error_code EC = llvm::sys::fs::createUniqueDirectory(
            "neverd-sanitize-capi", Unique)) {
      Error = EC.message();
      return;
    }
    Directory = Unique.c_str();
    Input = Directory / "input.bin";
    Output = Directory / "output.bin";
    if (Source.empty()) {
      if (!compileSource(NEVERD_SANITIZE_EMPTY_SOURCE, /*Relocatable=*/false,
                         /*NeedsSlack=*/false))
        return;
      return;
    }
    if (std::error_code EC =
            llvm::sys::fs::copy_file(Source.string(), Input.string()))
      Error = EC.message();
  }

  ~TemporaryFixture() {
    if (Directory.empty())
      return;
    std::error_code EC;
    std::filesystem::remove_all(Directory, EC);
  }

  bool valid() const { return Error.empty(); }
  bool compileRuntimeFixture() {
#ifdef _WIN32
    Input = Directory / "runtime-input.exe";
#else
    Input = Directory / "runtime-input";
#endif
    return compileSource(NEVERD_SANITIZE_RUNTIME_SOURCE,
                         /*Relocatable=*/false, /*NeedsSlack=*/true);
  }

  bool compileRelocatableFixture() {
#ifdef _WIN32
    Input = Directory / "runtime-empty.obj";
#else
    Input = Directory / "runtime-empty.o";
#endif
    return compileSource(NEVERD_SANITIZE_EMPTY_SOURCE,
                         /*Relocatable=*/true, /*NeedsSlack=*/false);
  }

  bool moveIntoBundle(llvm::StringRef Extension) {
    const std::filesystem::path Bundle =
        Directory / ("Container" + Extension.str()) / "Contents" / "MacOS";
    std::error_code EC;
    std::filesystem::create_directories(Bundle, EC);
    if (EC) {
      Error = EC.message();
      return false;
    }
    const std::filesystem::path BundledInput = Bundle / Input.filename();
    std::filesystem::rename(Input, BundledInput, EC);
    if (EC) {
      Error = EC.message();
      return false;
    }
    Input = BundledInput;
    return true;
  }

private:
  bool compileSource(llvm::StringRef SourcePath, bool Relocatable,
                     bool NeedsSlack) {
    const std::string Compiler = NEVERD_RUNTIME_FIXTURE_COMPILER;
    const std::string Source = SourcePath.str();
    const std::string Output = Input.string();
    if (Compiler.empty() || Source.empty()) {
      Error = "sanitizer fixture compiler or source is unavailable";
      return false;
    }
    std::vector<std::string> ArgumentStorage = {
        Compiler, "-O0", "-g", "-fno-builtin", "-fno-stack-protector"};
    if (Relocatable)
      ArgumentStorage.emplace_back("-c");
#ifdef __APPLE__
    if (!Relocatable) {
      // Exercise the only input signature profile accepted for automatic
      // publication: a linker's identityless signature with no requirements
      // or special-slot hashes. A later codesign invocation would synthesize
      // an empty requirements blob and therefore would not be a simple input.
      ArgumentStorage.emplace_back("-Wl,-adhoc_codesign");
      if (NeedsSlack)
        ArgumentStorage.emplace_back("-Wl,-no_compact_unwind");
    }
#endif
    ArgumentStorage.push_back(Source);
    ArgumentStorage.emplace_back("-o");
    ArgumentStorage.push_back(Output);
    std::vector<llvm::StringRef> Arguments;
    Arguments.reserve(ArgumentStorage.size());
    for (const std::string &Argument : ArgumentStorage)
      Arguments.emplace_back(Argument);
    std::string ExecutionError;
    const int Exit = llvm::sys::ExecuteAndWait(
        Compiler, Arguments, std::nullopt, {}, 0, 0, &ExecutionError);
    if (Exit != 0) {
      Error = "runtime fixture compile failed (" + std::to_string(Exit) +
              "): " + ExecutionError;
      return false;
    }
    return true;
  }

public:
  size_t sanitizerTemps() const {
    size_t Count = 0;
    for (const auto &Entry : std::filesystem::directory_iterator(Directory))
      if (Entry.path().filename().string().find(".neverd-sanitize-") !=
          std::string::npos)
        ++Count;
    return Count;
  }

  std::filesystem::path Directory;
  std::filesystem::path Input;
  std::filesystem::path Output;
  std::string Error;
};

class ScopedCurrentPath {
public:
  explicit ScopedCurrentPath(const std::filesystem::path &Target) {
    std::error_code EC;
    Original = std::filesystem::current_path(EC);
    if (EC) {
      Error = "cannot capture current path: " + EC.message();
      return;
    }
    std::filesystem::current_path(Target, EC);
    if (EC) {
      Error = "cannot change current path: " + EC.message();
      return;
    }
    Active = true;
  }

  ScopedCurrentPath(const ScopedCurrentPath &) = delete;
  ScopedCurrentPath &operator=(const ScopedCurrentPath &) = delete;

  ~ScopedCurrentPath() {
    if (!Active)
      return;
    std::error_code EC;
    std::filesystem::current_path(Original, EC);
  }

  bool valid() const { return Error.empty(); }

  std::string Error;

private:
  std::filesystem::path Original;
  bool Active = false;
};

void expectExactGuardPublish(uint32_t Strategy) {
  REQUIRE_RUNTIME_FIXTURE_COMPILER();
  TemporaryFixture Fixture;
  ASSERT_TRUE(Fixture.valid()) << Fixture.Error;
  ASSERT_TRUE(Fixture.compileRuntimeFixture()) << Fixture.Error;

  SessionGuard Session;
  ASSERT_EQ(neverd_session_load(Session.Value, Fixture.Input.string().c_str()),
            1)
      << takeString(neverd_last_error(Session.Value));
  neverd_sanitize_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  Options.strategy = Strategy;
  neverd_sanitize_result_v1 Result{};
  Result.struct_size = sizeof(Result);

  const int Succeeded = neverd_session_sanitize(
      Session.Value, Fixture.Output.string().c_str(), &Options, &Result);
#ifndef __APPLE__
  EXPECT_EQ(Succeeded, 0);
  EXPECT_EQ(Result.status, NEVERD_SANITIZE_STATUS_UNSUPPORTED_TARGET);
  EXPECT_EQ(Result.publication_outcome,
            NEVERD_SANITIZE_PUBLICATION_OUTCOME_NOT_ATTEMPTED);
  EXPECT_FALSE(std::filesystem::exists(Fixture.Output));
  return;
#endif
  ASSERT_EQ(Succeeded, 1) << neverd_sanitize_status_name(Result.status) << ": "
                          << takeString(neverd_last_error(Session.Value));
  EXPECT_EQ(Result.ok, 1);
  EXPECT_EQ(Result.status, NEVERD_SANITIZE_STATUS_OK);
  EXPECT_EQ(Result.plan_version, 1u);
  EXPECT_EQ(Result.findings, 1u);
  EXPECT_EQ(Result.guarded_sites, 1u);
  EXPECT_EQ(Result.guarded_functions, 1u);
  EXPECT_EQ(Result.unsupported_sites, 0u);
  EXPECT_GE(Result.patched_functions, Result.guarded_functions);
  EXPECT_GT(Result.code_size, 0u);
  EXPECT_GE(Result.trampoline_count, 1u);
  EXPECT_EQ(Result.publication_outcome,
            NEVERD_SANITIZE_PUBLICATION_OUTCOME_PUBLISHED);
  EXPECT_EQ(Result.publication_receipt_version,
            NEVERD_SANITIZE_PUBLICATION_ABI_VERSION);
  EXPECT_EQ(Result.publication_receipt_complete, 1u);
  EXPECT_EQ(Result.publication_namespace_disposition,
            NEVERD_SANITIZE_PUBLICATION_NAMESPACE_CREATE_EXCLUSIVE);
  EXPECT_EQ(
      Result.publication_guarantee_flags,
      NEVERD_SANITIZE_PUBLICATION_GUARANTEE_NAMESPACE_ATOMIC |
          NEVERD_SANITIZE_PUBLICATION_GUARANTEE_DESTINATION_CREATE_EXCLUSIVE);
  EXPECT_EQ(
      Result.publication_operand_binding,
      NEVERD_SANITIZE_PUBLICATION_OPERAND_BINDING_ACCESS_CONTROL_CONFINED_DISTINCT_CREDENTIALS);
  EXPECT_FALSE(readBytes(Fixture.Output).empty());
  EXPECT_EQ(takeString(neverd_patch_output_path(Session.Value)),
            std::filesystem::canonical(Fixture.Output).string());
  EXPECT_EQ(neverd_patch_code_size(Session.Value), Result.code_size);
  EXPECT_EQ(static_cast<uint64_t>(neverd_patch_trampoline_count(Session.Value)),
            Result.trampoline_count);
  EXPECT_EQ(Fixture.sanitizerTemps(), 0u);
#ifdef __APPLE__
  const std::string OutputPath = Fixture.Output.string();
  const std::vector<llvm::StringRef> Verify = {"/usr/bin/codesign", "--verify",
                                               "--strict", OutputPath};
  std::string VerifyError;
  EXPECT_EQ(runProgram(Verify, VerifyError), 0) << VerifyError;
#endif
}

} // namespace

TEST(NeverDSafetyPatchCAPI, InvalidSessionFailsClosedWithTypedStatus) {
  neverd_sanitize_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  neverd_sanitize_result_v1 Result{};
  Result.struct_size = sizeof(Result);
  Result.ok = 1;
  Result.findings = 99;

  EXPECT_EQ(
      neverd_session_sanitize(nullptr, "unused-output", &Options, &Result), 0);
  EXPECT_EQ(Result.struct_size, sizeof(Result));
  EXPECT_EQ(Result.ok, 0);
  EXPECT_EQ(Result.status, NEVERD_SANITIZE_STATUS_INVALID_SESSION);
  EXPECT_EQ(Result.plan_version, 0u);
  EXPECT_EQ(Result.findings, 0u);
  EXPECT_EQ(Result.guarded_sites, 0u);
  EXPECT_EQ(Result.guarded_functions, 0u);
  EXPECT_EQ(Result.unsupported_sites, 0u);
  EXPECT_EQ(Result.patched_functions, 0u);
  EXPECT_EQ(Result.code_size, 0u);
  EXPECT_EQ(Result.trampoline_count, 0u);
  EXPECT_EQ(Result.publication_outcome,
            NEVERD_SANITIZE_PUBLICATION_OUTCOME_NOT_ATTEMPTED);
  EXPECT_EQ(Result.publication_receipt_version,
            NEVERD_SANITIZE_PUBLICATION_ABI_VERSION);
  EXPECT_EQ(Result.publication_receipt_complete, 0u);
  EXPECT_EQ(Result.publication_namespace_disposition,
            NEVERD_SANITIZE_PUBLICATION_NAMESPACE_NONE);
  EXPECT_EQ(Result.publication_guarantee_flags, 0u);
  EXPECT_EQ(Result.publication_operand_binding,
            NEVERD_SANITIZE_PUBLICATION_OPERAND_BINDING_NONE);
}

TEST(NeverDSafetyPatchCAPI, NullOptionsFailBeforeAnalysisWithLastError) {
  SessionGuard Session;
  neverd_sanitize_result_v1 Result{};
  Result.struct_size = sizeof(Result);

  EXPECT_EQ(
      neverd_session_sanitize(Session.Value, "unused-output", nullptr, &Result),
      0);
  EXPECT_EQ(Result.status, NEVERD_SANITIZE_STATUS_INVALID_ARGUMENT);
  EXPECT_NE(takeString(neverd_last_error(Session.Value)).find("options"),
            std::string::npos);
}

TEST(NeverDSafetyPatchCAPI, ShortResultDoesNotOverwriteCallerStorage) {
  SessionGuard Session;
  neverd_sanitize_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  struct ShortResult {
    size_t struct_size;
    int ok;
    uint32_t canary;
  } Short{offsetof(ShortResult, canary), 7, 0x51a7c0deu};

  EXPECT_EQ(neverd_session_sanitize(
                Session.Value, "unused-output", &Options,
                reinterpret_cast<neverd_sanitize_result_v1 *>(&Short)),
            0);
  EXPECT_EQ(Short.struct_size, offsetof(ShortResult, canary));
  EXPECT_EQ(Short.ok, 0);
  EXPECT_EQ(Short.canary, 0x51a7c0deu);
  EXPECT_NE(takeString(neverd_last_error(Session.Value)).find("result"),
            std::string::npos);
}

TEST(NeverDSafetyPatchCAPI, MinimumResultPrefixWritesStatusOnlyWithinBounds) {
  SessionGuard Session;
  neverd_sanitize_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  struct ResultPrefix {
    size_t struct_size;
    int ok;
    uint32_t status;
    uint32_t canary;
  } Prefix{offsetof(ResultPrefix, canary), 7, 0xffffffffu, 0x51a7c0deu};

  EXPECT_EQ(neverd_session_sanitize(
                Session.Value, "unused-output", &Options,
                reinterpret_cast<neverd_sanitize_result_v1 *>(&Prefix)),
            0);
  EXPECT_EQ(Prefix.struct_size, offsetof(ResultPrefix, canary));
  EXPECT_EQ(Prefix.ok, 0);
  EXPECT_EQ(Prefix.status, NEVERD_SANITIZE_STATUS_NOT_LOADED);
  EXPECT_EQ(Prefix.canary, 0x51a7c0deu);
}

TEST(NeverDSafetyPatchCAPI, SplitKnownResultFieldFailsWithoutOverwritingIt) {
  SessionGuard Session;
  neverd_sanitize_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  neverd_sanitize_result_v1 Result{};
  Result.struct_size = offsetof(neverd_sanitize_result_v1, status) + 1;
  Result.ok = 7;
  Result.status = 0x51a7c0deu;

  EXPECT_EQ(neverd_session_sanitize(Session.Value, "unused-output", &Options,
                                    &Result),
            0);
  EXPECT_EQ(Result.struct_size,
            offsetof(neverd_sanitize_result_v1, status) + 1);
  EXPECT_EQ(Result.ok, 0);
  EXPECT_EQ(Result.status, 0x51a7c0deu);
  EXPECT_NE(takeString(neverd_last_error(Session.Value)).find("result"),
            std::string::npos);
}

TEST(NeverDSafetyPatchCAPI,
     EverySplitPublicationReceiptFieldFailsWithoutTailOverwrite) {
  SessionGuard Session;
  neverd_sanitize_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  constexpr std::array<size_t, 6> SplitSizes = {
      offsetof(neverd_sanitize_result_v1, publication_outcome) + 1,
      offsetof(neverd_sanitize_result_v1, publication_receipt_version) + 1,
      offsetof(neverd_sanitize_result_v1, publication_receipt_complete) + 1,
      offsetof(neverd_sanitize_result_v1, publication_namespace_disposition) +
          1,
      offsetof(neverd_sanitize_result_v1, publication_guarantee_flags) + 1,
      offsetof(neverd_sanitize_result_v1, publication_operand_binding) + 1,
  };
  for (size_t Size : SplitSizes) {
    alignas(neverd_sanitize_result_v1)
        std::array<uint8_t, sizeof(neverd_sanitize_result_v1) + 8>
            Storage;
    Storage.fill(0xa5);
    auto *Result =
        reinterpret_cast<neverd_sanitize_result_v1 *>(Storage.data());
    Result->struct_size = Size;
    const auto Before = Storage;

    EXPECT_EQ(neverd_session_sanitize(Session.Value, "unused-output", &Options,
                                      Result),
              0)
        << "split size " << Size;
    EXPECT_EQ(Result->struct_size, Size);
    EXPECT_TRUE(std::equal(Storage.begin() + Size, Storage.end(),
                           Before.begin() + Size))
        << "split size " << Size;
  }
}

TEST(NeverDSafetyPatchCAPI,
     OldAndEveryCompletePublicationReceiptPrefixRemainAccepted) {
  SessionGuard Session;
  neverd_sanitize_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  constexpr std::array<size_t, 7> PrefixSizes = {
      offsetof(neverd_sanitize_result_v1, publication_outcome),
      offsetof(neverd_sanitize_result_v1, publication_outcome) +
          sizeof(neverd_sanitize_result_v1::publication_outcome),
      offsetof(neverd_sanitize_result_v1, publication_receipt_version) +
          sizeof(neverd_sanitize_result_v1::publication_receipt_version),
      offsetof(neverd_sanitize_result_v1, publication_receipt_complete) +
          sizeof(neverd_sanitize_result_v1::publication_receipt_complete),
      offsetof(neverd_sanitize_result_v1, publication_namespace_disposition) +
          sizeof(neverd_sanitize_result_v1::publication_namespace_disposition),
      offsetof(neverd_sanitize_result_v1, publication_guarantee_flags) +
          sizeof(neverd_sanitize_result_v1::publication_guarantee_flags),
      sizeof(neverd_sanitize_result_v1),
  };
  for (size_t Size : PrefixSizes) {
    neverd_sanitize_result_v1 Result{};
    Result.struct_size = Size;
    EXPECT_EQ(neverd_session_sanitize(Session.Value, "unused-output", &Options,
                                      &Result),
              0)
        << "prefix size " << Size;
    EXPECT_EQ(Result.status, NEVERD_SANITIZE_STATUS_NOT_LOADED);
    if (Size >=
        offsetof(neverd_sanitize_result_v1, publication_receipt_version) +
            sizeof(Result.publication_receipt_version))
      EXPECT_EQ(Result.publication_receipt_version,
                NEVERD_SANITIZE_PUBLICATION_ABI_VERSION);
  }
}

TEST(NeverDSafetyPatchCAPI, UnknownResultTailIsNeverWritten) {
  SessionGuard Session;
  neverd_sanitize_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  struct ExtendedResult {
    neverd_sanitize_result_v1 Base;
    uint64_t Future;
  } Result{};
  Result.Base.struct_size = sizeof(Result);
  Result.Future = 0x51a7c0debaadf00dULL;

  EXPECT_EQ(neverd_session_sanitize(Session.Value, "unused-output", &Options,
                                    &Result.Base),
            0);
  EXPECT_EQ(Result.Base.status, NEVERD_SANITIZE_STATUS_NOT_LOADED);
  EXPECT_EQ(Result.Base.publication_receipt_version,
            NEVERD_SANITIZE_PUBLICATION_ABI_VERSION);
  EXPECT_EQ(Result.Future, 0x51a7c0debaadf00dULL);
}

TEST(NeverDSafetyPatchCAPI, StatusNamesAreStableAndRejectUnknownValues) {
  const char *Expected[] = {
      "ok",
      "invalid-argument",
      "invalid-session",
      "not-loaded",
      "unsupported-target",
      "pipeline-failed",
      "incomplete-coverage",
      "hunt-incomplete",
      "metadata-invalid",
      "plan-incomplete",
      "guard-failed",
      "io-failed",
      "patch-failed",
      "receipt-mismatch",
      "reload-failed",
      "authentication-failed",
      "publish-failed",
      "signature-unsupported",
      "signing-failed",
      "publish-indeterminate",
      "published-incomplete",
  };
  for (uint32_t Status = NEVERD_SANITIZE_STATUS_OK;
       Status <= NEVERD_SANITIZE_STATUS_PUBLISHED_INCOMPLETE; ++Status)
    EXPECT_STREQ(neverd_sanitize_status_name(Status), Expected[Status]);
  EXPECT_STREQ(neverd_sanitize_status_name(0xffffffffu), "invalid");
}

TEST(NeverDSafetyPatchCAPI, PublicationABIVersionIsStableAndProbeable) {
  EXPECT_EQ(neverd_sanitize_publication_abi_version(),
            NEVERD_SANITIZE_PUBLICATION_ABI_VERSION);
  EXPECT_EQ(neverd_sanitize_publication_abi_version(), 1u);
}

TEST(NeverDSafetyPatchCAPI, StructSizeOnlyOptionsUseDefaults) {
  SessionGuard Session;
  neverd_sanitize_options_v1 Options;
  std::memset(&Options, 0xa5, sizeof(Options));
  Options.struct_size = sizeof(size_t);
  neverd_sanitize_result_v1 Result{};
  Result.struct_size = sizeof(Result);

  EXPECT_EQ(neverd_session_sanitize(Session.Value, "unused-output", &Options,
                                    &Result),
            0);
  EXPECT_EQ(Result.status, NEVERD_SANITIZE_STATUS_NOT_LOADED);
  EXPECT_NE(takeString(neverd_last_error(Session.Value)).find("loaded"),
            std::string::npos);
}

TEST(NeverDSafetyPatchCAPI, SplitKnownOptionFieldFailsClosed) {
  SessionGuard Session;
  neverd_sanitize_options_v1 Options{};
  Options.struct_size = offsetof(neverd_sanitize_options_v1, strategy) + 1;
  Options.strategy = NEVERD_SANITIZE_STRATEGY_INPLACE;
  neverd_sanitize_result_v1 Result{};
  Result.struct_size = sizeof(Result);

  EXPECT_EQ(neverd_session_sanitize(Session.Value, "unused-output", &Options,
                                    &Result),
            0);
  EXPECT_EQ(Result.status, NEVERD_SANITIZE_STATUS_INVALID_ARGUMENT);
  EXPECT_NE(takeString(neverd_last_error(Session.Value)).find("struct_size"),
            std::string::npos);
}

TEST(NeverDSafetyPatchCAPI, InvalidStrategyAndEmptyOutputFailLocally) {
  SessionGuard Session;
  neverd_sanitize_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  Options.strategy = 99;
  neverd_sanitize_result_v1 Result{};
  Result.struct_size = sizeof(Result);

  EXPECT_EQ(neverd_session_sanitize(Session.Value, "unused", &Options, &Result),
            0);
  EXPECT_EQ(Result.status, NEVERD_SANITIZE_STATUS_INVALID_ARGUMENT);
  EXPECT_NE(takeString(neverd_last_error(Session.Value)).find("strategy"),
            std::string::npos);

  Options.strategy = NEVERD_SANITIZE_STRATEGY_SECTION;
  EXPECT_EQ(neverd_session_sanitize(Session.Value, nullptr, &Options, &Result),
            0);
  EXPECT_EQ(Result.status, NEVERD_SANITIZE_STATUS_INVALID_ARGUMENT);
  EXPECT_NE(takeString(neverd_last_error(Session.Value)).find("output"),
            std::string::npos);
}

TEST(NeverDSafetyPatchCAPI, UnknownOptionTailIsIgnored) {
  SessionGuard Session;
  struct ExtendedOptions {
    neverd_sanitize_options_v1 Base;
    uint64_t Future;
  } Options{};
  Options.Base.struct_size = sizeof(Options);
  Options.Future = 0xffffffffffffffffULL;
  neverd_sanitize_result_v1 Result{};
  Result.struct_size = sizeof(Result);

  EXPECT_EQ(
      neverd_session_sanitize(Session.Value, "unused", &Options.Base, &Result),
      0);
  EXPECT_EQ(Result.status, NEVERD_SANITIZE_STATUS_NOT_LOADED);
}

TEST(NeverDSafetyPatchCAPI,
     LoadPreservesPublicLocatorAndCapturesCanonicalSanitizerSource) {
  REQUIRE_RUNTIME_FIXTURE_COMPILER();
  TemporaryFixture Fixture;
  ASSERT_TRUE(Fixture.valid()) << Fixture.Error;
  const std::filesystem::path CallerPath =
      Fixture.Directory / "." / Fixture.Input.filename();
  ASSERT_NE(CallerPath.string(),
            std::filesystem::canonical(CallerPath).string());

  SessionGuard Session;
  ASSERT_EQ(neverd_session_load(Session.Value, CallerPath.string().c_str()), 1)
      << takeString(neverd_last_error(Session.Value));
  EXPECT_EQ(takeString(neverd_session_file_path(Session.Value)),
            CallerPath.string());
  const auto *Internal =
      reinterpret_cast<const neverd::sdk::Session *>(Session.Value);
  EXPECT_EQ(Internal->SanitizeSourcePath,
            std::filesystem::canonical(CallerPath));
}

TEST(NeverDSafetyPatchCAPI,
     RelativeDestinationIsResolvedOnceBeforeAmbientPathCanChange) {
  REQUIRE_RUNTIME_FIXTURE_COMPILER();
  TemporaryFixture Fixture;
  ASSERT_TRUE(Fixture.valid()) << Fixture.Error;
  const std::filesystem::path First = Fixture.Directory / "first";
  const std::filesystem::path Second = Fixture.Directory / "second";
  std::error_code EC;
  ASSERT_TRUE(std::filesystem::create_directories(First, EC)) << EC.message();
  EC.clear();
  ASSERT_TRUE(std::filesystem::create_directories(Second, EC)) << EC.message();
  const std::filesystem::path ExpectedDestination =
      std::filesystem::canonical(First) / "relative-output.bin";

  SessionGuard Session;
  ASSERT_EQ(neverd_session_load(Session.Value, Fixture.Input.string().c_str()),
            1)
      << takeString(neverd_last_error(Session.Value));
  ScopedCurrentPath CurrentPath(First);
  ASSERT_TRUE(CurrentPath.valid()) << CurrentPath.Error;

  std::filesystem::path ObservedDestination;
  auto *Internal = reinterpret_cast<neverd::sdk::Session *>(Session.Value);
  Internal->SanitizeAfterDestinationResolutionForTesting =
      [&](const std::filesystem::path &Resolved) {
        ObservedDestination = Resolved;
        std::filesystem::current_path(Second);
      };

  neverd_sanitize_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  neverd_sanitize_result_v1 Result{};
  Result.struct_size = sizeof(Result);
  const int Succeeded = neverd_session_sanitize(
      Session.Value, "relative-output.bin", &Options, &Result);

  EXPECT_EQ(ObservedDestination, ExpectedDestination);
#ifdef __APPLE__
  EXPECT_EQ(Succeeded, 1) << takeString(neverd_last_error(Session.Value));
  EXPECT_TRUE(std::filesystem::exists(First / "relative-output.bin"));
#else
  EXPECT_EQ(Succeeded, 0);
  EXPECT_EQ(Result.status, NEVERD_SANITIZE_STATUS_UNSUPPORTED_TARGET);
#endif
  EXPECT_FALSE(std::filesystem::exists(Second / "relative-output.bin"));
}

TEST(NeverDSafetyPatchCAPI,
     GuardedRelativeSourceNeverReinterpretsLegacyLocatorAfterLoad) {
#ifndef __APPLE__
  GTEST_SKIP() << "guarded Mach-O source policy is Darwin-specific";
#else
  REQUIRE_RUNTIME_FIXTURE_COMPILER();
  TemporaryFixture Fixture;
  ASSERT_TRUE(Fixture.valid()) << Fixture.Error;
  ASSERT_TRUE(Fixture.compileRuntimeFixture()) << Fixture.Error;

  const std::filesystem::path First = Fixture.Directory;
  const std::filesystem::path Second = Fixture.Directory / "second";
  const std::filesystem::path BundleBin =
      Second / "Container.app" / "Contents" / "MacOS";
  std::error_code EC;
  std::filesystem::create_directories(BundleBin, EC);
  ASSERT_FALSE(EC) << EC.message();
  // The legacy caller spelling contains a parent component but resolves to the
  // fixture's original location, keeping all debug companions discoverable.
  std::filesystem::create_directory_symlink(First, First / "bin", EC);
  ASSERT_FALSE(EC) << EC.message();
  std::filesystem::create_directory_symlink(BundleBin, Second / "bin", EC);
  ASSERT_FALSE(EC) << EC.message();

  ScopedCurrentPath CurrentPath(First);
  ASSERT_TRUE(CurrentPath.valid()) << CurrentPath.Error;
  const std::filesystem::path CallerPath =
      std::filesystem::path("bin") / Fixture.Input.filename();
  SessionGuard Session;
  ASSERT_EQ(neverd_session_load(Session.Value, CallerPath.string().c_str()), 1)
      << takeString(neverd_last_error(Session.Value));
  EXPECT_EQ(takeString(neverd_session_file_path(Session.Value)),
            CallerPath.string());

  std::filesystem::current_path(Second, EC);
  ASSERT_FALSE(EC) << EC.message();
  neverd_sanitize_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  neverd_sanitize_result_v1 Result{};
  Result.struct_size = sizeof(Result);
  const int Succeeded = neverd_session_sanitize(
      Session.Value, Fixture.Output.string().c_str(), &Options, &Result);

  EXPECT_EQ(Succeeded, 1) << takeString(neverd_last_error(Session.Value));
  EXPECT_EQ(Result.status, NEVERD_SANITIZE_STATUS_OK);
  EXPECT_EQ(Result.publication_outcome,
            NEVERD_SANITIZE_PUBLICATION_OUTCOME_PUBLISHED);
  EXPECT_TRUE(std::filesystem::exists(Fixture.Output));
#endif
}

TEST(NeverDSafetyPatchCAPI, EmptyStrictPlanPublishesByteIdenticalCopy) {
  REQUIRE_RUNTIME_FIXTURE_COMPILER();
  TemporaryFixture Fixture;
  ASSERT_TRUE(Fixture.valid()) << Fixture.Error;
  const std::vector<uint8_t> Original = readBytes(Fixture.Input);
  ASSERT_FALSE(Original.empty());

  SessionGuard Session;
  ASSERT_EQ(neverd_session_load(Session.Value, Fixture.Input.string().c_str()),
            1)
      << takeString(neverd_last_error(Session.Value));
  neverd_sanitize_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  neverd_sanitize_result_v1 Result{};
  Result.struct_size = sizeof(Result);

  const int Succeeded = neverd_session_sanitize(
      Session.Value, Fixture.Output.string().c_str(), &Options, &Result);
#ifndef __APPLE__
  EXPECT_EQ(Succeeded, 0);
  EXPECT_EQ(Result.status, NEVERD_SANITIZE_STATUS_UNSUPPORTED_TARGET);
  EXPECT_EQ(Result.publication_outcome,
            NEVERD_SANITIZE_PUBLICATION_OUTCOME_NOT_ATTEMPTED);
  EXPECT_EQ(Result.publication_receipt_version,
            NEVERD_SANITIZE_PUBLICATION_ABI_VERSION);
  EXPECT_FALSE(std::filesystem::exists(Fixture.Output));
  return;
#endif
  ASSERT_EQ(Succeeded, 1) << takeString(neverd_last_error(Session.Value));
  EXPECT_EQ(Result.ok, 1);
  EXPECT_EQ(Result.status, NEVERD_SANITIZE_STATUS_OK);
  EXPECT_EQ(Result.plan_version, 1u);
  EXPECT_EQ(Result.findings, 0u);
  EXPECT_EQ(Result.guarded_sites, 0u);
  EXPECT_EQ(Result.guarded_functions, 0u);
  EXPECT_EQ(Result.unsupported_sites, 0u);
  EXPECT_EQ(Result.patched_functions, 0u);
  EXPECT_EQ(Result.code_size, 0u);
  EXPECT_EQ(Result.trampoline_count, 0u);
  EXPECT_EQ(Result.publication_outcome,
            NEVERD_SANITIZE_PUBLICATION_OUTCOME_PUBLISHED);
  EXPECT_EQ(Result.publication_receipt_version,
            NEVERD_SANITIZE_PUBLICATION_ABI_VERSION);
  EXPECT_EQ(Result.publication_receipt_complete, 1u);
  EXPECT_EQ(Result.publication_namespace_disposition,
            NEVERD_SANITIZE_PUBLICATION_NAMESPACE_CREATE_EXCLUSIVE);
  EXPECT_EQ(
      Result.publication_guarantee_flags,
      NEVERD_SANITIZE_PUBLICATION_GUARANTEE_NAMESPACE_ATOMIC |
          NEVERD_SANITIZE_PUBLICATION_GUARANTEE_DESTINATION_CREATE_EXCLUSIVE);
  EXPECT_EQ(readBytes(Fixture.Output), Original);
  EXPECT_EQ(takeString(neverd_patch_output_path(Session.Value)),
            std::filesystem::canonical(Fixture.Output).string());
  EXPECT_EQ(neverd_patch_code_size(Session.Value), 0u);
  EXPECT_EQ(neverd_patch_trampoline_count(Session.Value), 0);
  EXPECT_EQ(Fixture.sanitizerTemps(), 0u);
}

TEST(NeverDSafetyPatchCAPI,
     ChangedSourcePreservesDestinationAndPriorSessionResults) {
  REQUIRE_RUNTIME_FIXTURE_COMPILER();
  TemporaryFixture Fixture;
  ASSERT_TRUE(Fixture.valid()) << Fixture.Error;
  const std::vector<uint8_t> LoadedBytes = readBytes(Fixture.Input);
  ASSERT_FALSE(LoadedBytes.empty());
  const std::vector<uint8_t> DestinationBytes = {'p', 'r', 'i', 'o', 'r'};
  writeBytes(Fixture.Output, DestinationBytes);

  SessionGuard Session;
  ASSERT_EQ(neverd_session_load(Session.Value, Fixture.Input.string().c_str()),
            1)
      << takeString(neverd_last_error(Session.Value));
  auto *Internal = neverd::sdk::toSession(Session.Value);
  seedPriorSessionResults(*Internal);

  std::vector<uint8_t> Changed = LoadedBytes;
  Changed.back() ^= 0x5a;
  writeBytes(Fixture.Input, Changed);
  neverd_sanitize_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  neverd_sanitize_result_v1 Result{};
  Result.struct_size = sizeof(Result);

  EXPECT_EQ(neverd_session_sanitize(Session.Value,
                                    Fixture.Output.string().c_str(), &Options,
                                    &Result),
            0);
  EXPECT_EQ(Result.status, NEVERD_SANITIZE_STATUS_AUTHENTICATION_FAILED);
  EXPECT_NE(takeString(neverd_last_error(Session.Value)).find("changed"),
            std::string::npos);
  EXPECT_EQ(readBytes(Fixture.Output), DestinationBytes);
  EXPECT_EQ(Fixture.sanitizerTemps(), 0u);
  expectPriorSessionResults(Session.Value);
}

TEST(NeverDSafetyPatchCAPI,
     EmptyPlanAuthenticatesItsSourceWithoutNamespaceMutation) {
  REQUIRE_RUNTIME_FIXTURE_COMPILER();
  TemporaryFixture Fixture;
  ASSERT_TRUE(Fixture.valid()) << Fixture.Error;
  const std::vector<uint8_t> Original = readBytes(Fixture.Input);
  SessionGuard Session;
  ASSERT_EQ(neverd_session_load(Session.Value, Fixture.Input.string().c_str()),
            1)
      << takeString(neverd_last_error(Session.Value));
  neverd_sanitize_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  neverd_sanitize_result_v1 Result{};
  Result.struct_size = sizeof(Result);

  const int Succeeded = neverd_session_sanitize(
      Session.Value, Fixture.Input.string().c_str(), &Options, &Result);
#ifndef __APPLE__
  EXPECT_EQ(Succeeded, 0);
  EXPECT_EQ(Result.status, NEVERD_SANITIZE_STATUS_UNSUPPORTED_TARGET);
  EXPECT_EQ(Result.publication_outcome,
            NEVERD_SANITIZE_PUBLICATION_OUTCOME_NOT_ATTEMPTED);
  EXPECT_EQ(readBytes(Fixture.Input), Original);
  return;
#endif
  ASSERT_EQ(Succeeded, 1) << takeString(neverd_last_error(Session.Value));
  EXPECT_EQ(Result.status, NEVERD_SANITIZE_STATUS_OK);
  EXPECT_EQ(Result.publication_outcome,
            NEVERD_SANITIZE_PUBLICATION_OUTCOME_NOT_PUBLISHED);
  EXPECT_EQ(Result.publication_receipt_version,
            NEVERD_SANITIZE_PUBLICATION_ABI_VERSION);
  EXPECT_EQ(Result.publication_receipt_complete, 1u);
  EXPECT_EQ(Result.publication_namespace_disposition,
            NEVERD_SANITIZE_PUBLICATION_NAMESPACE_NO_CHANGE);
  EXPECT_EQ(Result.publication_guarantee_flags, 0u);
  EXPECT_EQ(Result.publication_operand_binding,
            NEVERD_SANITIZE_PUBLICATION_OPERAND_BINDING_NONE);
  EXPECT_EQ(readBytes(Fixture.Input), Original);
  EXPECT_EQ(takeString(neverd_patch_output_path(Session.Value)),
            std::filesystem::canonical(Fixture.Input).string());
  EXPECT_EQ(Fixture.sanitizerTemps(), 0u);
}

#ifdef __APPLE__
TEST(NeverDSafetyPatchCAPI,
     ExistingDistinctDestinationIsNeverReplacedWithoutCAS) {
  REQUIRE_RUNTIME_FIXTURE_COMPILER();
  TemporaryFixture Fixture;
  ASSERT_TRUE(Fixture.valid()) << Fixture.Error;
  const std::vector<uint8_t> DestinationBytes = {'p', 'r', 'i', 'o', 'r'};
  writeBytes(Fixture.Output, DestinationBytes);

  SessionGuard Session;
  ASSERT_EQ(neverd_session_load(Session.Value, Fixture.Input.string().c_str()),
            1)
      << takeString(neverd_last_error(Session.Value));
  auto *Internal = neverd::sdk::toSession(Session.Value);
  seedPriorSessionResults(*Internal);
  neverd_sanitize_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  neverd_sanitize_result_v1 Result{};
  Result.struct_size = sizeof(Result);

  EXPECT_EQ(neverd_session_sanitize(Session.Value,
                                    Fixture.Output.string().c_str(), &Options,
                                    &Result),
            0);
  EXPECT_EQ(Result.status, NEVERD_SANITIZE_STATUS_PUBLISH_FAILED);
  EXPECT_EQ(Result.publication_outcome,
            NEVERD_SANITIZE_PUBLICATION_OUTCOME_NOT_ATTEMPTED);
  EXPECT_EQ(Result.publication_receipt_version,
            NEVERD_SANITIZE_PUBLICATION_ABI_VERSION);
  EXPECT_EQ(Result.publication_receipt_complete, 0u);
  EXPECT_EQ(Result.publication_namespace_disposition,
            NEVERD_SANITIZE_PUBLICATION_NAMESPACE_NONE);
  EXPECT_NE(
      takeString(neverd_last_error(Session.Value)).find("replacement CAS"),
      std::string::npos);
  EXPECT_EQ(readBytes(Fixture.Output), DestinationBytes);
  EXPECT_EQ(Fixture.sanitizerTemps(), 0u);
  expectPriorSessionResults(Session.Value);
}

TEST(NeverDSafetyPatchCAPI,
     PublicationOutcomeDistinguishesIndeterminateAndPublishedIncomplete) {
  REQUIRE_RUNTIME_FIXTURE_COMPILER();
  TemporaryFixture Fixture;
  ASSERT_TRUE(Fixture.valid()) << Fixture.Error;
  ASSERT_TRUE(Fixture.compileRuntimeFixture()) << Fixture.Error;
  SessionGuard Session;
  ASSERT_EQ(neverd_session_load(Session.Value, Fixture.Input.string().c_str()),
            1)
      << takeString(neverd_last_error(Session.Value));
  auto *Internal = neverd::sdk::toSession(Session.Value);
  seedPriorSessionResults(*Internal);
  neverd_sanitize_options_v1 Options{};
  Options.struct_size = sizeof(Options);

  const auto RunFault = [&](auto Fault, llvm::StringRef Name,
                            neverd_sanitize_status_t ExpectedStatus,
                            neverd_sanitize_publication_outcome_t
                                ExpectedOutcome,
                            bool DestinationExists,
                            bool ExpectDiagnosticException) {
    const std::filesystem::path Destination =
        Fixture.Directory / (Name.str() + ".bin");
    Internal->SanitizePublicationFault = Fault;
    neverd_sanitize_result_v1 Result{};
    Result.struct_size = sizeof(Result);
    EXPECT_EQ(neverd_session_sanitize(Session.Value,
                                      Destination.string().c_str(), &Options,
                                      &Result),
              0);
    EXPECT_EQ(Result.ok, 0);
    EXPECT_EQ(Result.status, ExpectedStatus);
    EXPECT_EQ(Result.publication_outcome, ExpectedOutcome);
    EXPECT_EQ(Result.publication_receipt_version,
              NEVERD_SANITIZE_PUBLICATION_ABI_VERSION);
    EXPECT_EQ(Result.publication_receipt_complete, 0u);
    EXPECT_EQ(Result.publication_namespace_disposition,
              NEVERD_SANITIZE_PUBLICATION_NAMESPACE_CREATE_EXCLUSIVE);
    EXPECT_EQ(Result.publication_guarantee_flags, 0u);
    EXPECT_EQ(Result.publication_operand_binding,
              NEVERD_SANITIZE_PUBLICATION_OPERAND_BINDING_NONE);
    const std::string Failure = takeString(neverd_last_error(Session.Value));
    EXPECT_EQ(
        Failure.find(
            "injected post-publication diagnostic construction exception") !=
            std::string::npos,
        ExpectDiagnosticException);
    EXPECT_EQ(std::filesystem::exists(Destination), DestinationExists);
    EXPECT_EQ(Fixture.sanitizerTemps(), 0u);
    expectPriorSessionResults(Session.Value);
  };

  RunFault(neverd::sdk::Session::SanitizePublicationFaultForTesting::
               PublishIndeterminate,
           "indeterminate", NEVERD_SANITIZE_STATUS_PUBLISH_INDETERMINATE,
           NEVERD_SANITIZE_PUBLICATION_OUTCOME_INDETERMINATE,
           /*DestinationExists=*/false,
           /*ExpectDiagnosticException=*/true);
  RunFault(neverd::sdk::Session::SanitizePublicationFaultForTesting::
               PublishedFinalAuthenticationFailure,
           "final-auth", NEVERD_SANITIZE_STATUS_PUBLISHED_INCOMPLETE,
           NEVERD_SANITIZE_PUBLICATION_OUTCOME_PUBLISHED,
           /*DestinationExists=*/true,
           /*ExpectDiagnosticException=*/true);
  RunFault(neverd::sdk::Session::SanitizePublicationFaultForTesting::
               PublishedFinalizationFailure,
           "finalize", NEVERD_SANITIZE_STATUS_PUBLISHED_INCOMPLETE,
           NEVERD_SANITIZE_PUBLICATION_OUTCOME_PUBLISHED,
           /*DestinationExists=*/true,
           /*ExpectDiagnosticException=*/false);
}
#endif

#ifdef __APPLE__
TEST(NeverDSafetyPatchCAPI,
     IncompletePlanReportsDiagnosticsWithoutPublishingOrStateCommit) {
  TemporaryFixture Fixture(std::filesystem::path(NEVERD_SAFETY_FIXTURE_ROOT) /
                           "safety_cases_macho_x64");
  ASSERT_TRUE(Fixture.valid()) << Fixture.Error;
  const std::vector<uint8_t> DestinationBytes = {'o', 'l', 'd'};
  writeBytes(Fixture.Output, DestinationBytes);
  SessionGuard Session;
  ASSERT_EQ(neverd_session_load(Session.Value, Fixture.Input.string().c_str()),
            1)
      << takeString(neverd_last_error(Session.Value));
  auto *Internal = neverd::sdk::toSession(Session.Value);
  Internal->LastPatch.Success = true;
  Internal->LastPatch.OutputPath = "previous";
  Internal->LastPatch.CodeSize = 77;

  neverd_sanitize_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  neverd_sanitize_result_v1 Result{};
  Result.struct_size = sizeof(Result);
  EXPECT_EQ(neverd_session_sanitize(Session.Value,
                                    Fixture.Output.string().c_str(), &Options,
                                    &Result),
            0);
  EXPECT_EQ(Result.status, NEVERD_SANITIZE_STATUS_PLAN_INCOMPLETE);
  EXPECT_EQ(Result.plan_version, 1u);
  EXPECT_GT(Result.findings, 0u);
  EXPECT_GT(Result.unsupported_sites, 0u);
  EXPECT_NE(takeString(neverd_last_error(Session.Value)).find("incomplete"),
            std::string::npos);
  EXPECT_EQ(readBytes(Fixture.Output), DestinationBytes);
  EXPECT_EQ(takeString(neverd_patch_output_path(Session.Value)), "previous");
  EXPECT_EQ(neverd_patch_code_size(Session.Value), 77u);
  EXPECT_EQ(Fixture.sanitizerTemps(), 0u);
}
#endif

TEST(NeverDSafetyPatchCAPI, SectionPublishesOneExactCountedWriteGuard) {
  expectExactGuardPublish(NEVERD_SANITIZE_STRATEGY_SECTION);
}

TEST(NeverDSafetyPatchCAPI, InplacePublishesOneExactCountedWriteGuard) {
  expectExactGuardPublish(NEVERD_SANITIZE_STRATEGY_INPLACE);
}

TEST(NeverDSafetyPatchCAPI,
     RelocatableObjectFailsTargetGateBeforePipelineOrPublication) {
  REQUIRE_RUNTIME_FIXTURE_COMPILER();
  TemporaryFixture Fixture;
  ASSERT_TRUE(Fixture.valid()) << Fixture.Error;
  ASSERT_TRUE(Fixture.compileRelocatableFixture()) << Fixture.Error;
  SessionGuard Session;
  ASSERT_EQ(neverd_session_load(Session.Value, Fixture.Input.string().c_str()),
            1)
      << takeString(neverd_last_error(Session.Value));
  neverd_sanitize_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  neverd_sanitize_result_v1 Result{};
  Result.struct_size = sizeof(Result);

  EXPECT_EQ(neverd_session_sanitize(Session.Value,
                                    Fixture.Output.string().c_str(), &Options,
                                    &Result),
            0);
  EXPECT_EQ(Result.status, NEVERD_SANITIZE_STATUS_UNSUPPORTED_TARGET);
  EXPECT_EQ(Result.plan_version, 0u);
  EXPECT_NE(takeString(neverd_last_error(Session.Value)).find("relocatable"),
            std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(Fixture.Output));
}

TEST(NeverDSafetyPatchCAPI,
     DynamicLibrariesFailTargetGateBeforePipelineOrPublication) {
  for (const char *Name : {"lowir_concolic_elf_x64", "lowir_concolic_macho_x64",
                           "lowir_concolic_pe_x64"}) {
    SCOPED_TRACE(Name);
    TemporaryFixture Fixture(
        std::filesystem::path(NEVERD_SANITIZE_FIXTURE_ROOT) / Name);
    ASSERT_TRUE(Fixture.valid()) << Fixture.Error;
    SessionGuard Session;
    ASSERT_EQ(
        neverd_session_load(Session.Value, Fixture.Input.string().c_str()), 1)
        << takeString(neverd_last_error(Session.Value));
    neverd_sanitize_options_v1 Options{};
    Options.struct_size = sizeof(Options);
    neverd_sanitize_result_v1 Result{};
    Result.struct_size = sizeof(Result);

    EXPECT_EQ(neverd_session_sanitize(Session.Value,
                                      Fixture.Output.string().c_str(), &Options,
                                      &Result),
              0);
    EXPECT_EQ(Result.status, NEVERD_SANITIZE_STATUS_UNSUPPORTED_TARGET);
    EXPECT_EQ(Result.plan_version, 0u);
    EXPECT_NE(
        takeString(neverd_last_error(Session.Value)).find("dynamic library"),
        std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(Fixture.Output));
  }
}

TEST(NeverDSafetyPatchCAPI,
     UniversalMachOFailsBeforeHostSliceSourceAuthentication) {
  REQUIRE_RUNTIME_FIXTURE_COMPILER();
  TemporaryFixture Fixture;
  ASSERT_TRUE(Fixture.valid()) << Fixture.Error;
  const std::filesystem::path Root(NEVERD_SAFETY_FIXTURE_ROOT);
  const std::vector<uint8_t> X64 = readBytes(Root / "safety_cases_macho_x64");
  const std::vector<uint8_t> ARM64 =
      readBytes(Root / "safety_cases_macho_arm64");
  ASSERT_FALSE(X64.empty());
  ASSERT_FALSE(ARM64.empty());
  writeBytes(Fixture.Input, makeUniversalMachO(X64, ARM64));
  SessionGuard Session;
  ASSERT_EQ(neverd_session_load(Session.Value, Fixture.Input.string().c_str()),
            1)
      << takeString(neverd_last_error(Session.Value));
  neverd_sanitize_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  neverd_sanitize_result_v1 Result{};
  Result.struct_size = sizeof(Result);

  EXPECT_EQ(neverd_session_sanitize(Session.Value,
                                    Fixture.Output.string().c_str(), &Options,
                                    &Result),
            0);
  EXPECT_EQ(Result.status, NEVERD_SANITIZE_STATUS_UNSUPPORTED_TARGET);
  EXPECT_EQ(Result.plan_version, 0u);
  EXPECT_NE(takeString(neverd_last_error(Session.Value))
                .find("universal Mach-O publication is unsupported"),
            std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(Fixture.Output));
}

TEST(NeverDSafetyPatchCAPI,
     DestinationInsideMissingMixedCaseBundleFailsEvenForNoOpPlan) {
  REQUIRE_RUNTIME_FIXTURE_COMPILER();
  TemporaryFixture Fixture;
  ASSERT_TRUE(Fixture.valid()) << Fixture.Error;
  Fixture.Output =
      Fixture.Directory / "Container.ApP" / "Contents" / "MacOS" / "new-member";
  SessionGuard Session;
  ASSERT_EQ(neverd_session_load(Session.Value, Fixture.Input.string().c_str()),
            1)
      << takeString(neverd_last_error(Session.Value));
  neverd_sanitize_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  neverd_sanitize_result_v1 Result{};
  Result.struct_size = sizeof(Result);

  EXPECT_EQ(neverd_session_sanitize(Session.Value,
                                    Fixture.Output.string().c_str(), &Options,
                                    &Result),
            0);
  EXPECT_EQ(Result.status, NEVERD_SANITIZE_STATUS_SIGNATURE_UNSUPPORTED);
  EXPECT_EQ(Result.plan_version, 0u);
  EXPECT_NE(takeString(neverd_last_error(Session.Value)).find("destination"),
            std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(Fixture.Output));
}

#ifdef __APPLE__
TEST(NeverDSafetyPatchCAPI,
     SourceBundleMemberNoOpMayCopyOutsideAndPreserveSignature) {
  REQUIRE_RUNTIME_FIXTURE_COMPILER();
  TemporaryFixture Fixture;
  ASSERT_TRUE(Fixture.valid()) << Fixture.Error;
  ASSERT_TRUE(Fixture.moveIntoBundle(".ApP")) << Fixture.Error;
  SessionGuard Session;
  ASSERT_EQ(neverd_session_load(Session.Value, Fixture.Input.string().c_str()),
            1)
      << takeString(neverd_last_error(Session.Value));
  neverd_sanitize_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  neverd_sanitize_result_v1 Result{};
  Result.struct_size = sizeof(Result);

  ASSERT_EQ(neverd_session_sanitize(Session.Value,
                                    Fixture.Output.string().c_str(), &Options,
                                    &Result),
            1)
      << takeString(neverd_last_error(Session.Value));
  EXPECT_EQ(Result.status, NEVERD_SANITIZE_STATUS_OK);
  const std::string OutputPath = Fixture.Output.string();
  const std::vector<llvm::StringRef> Verify = {"/usr/bin/codesign", "--verify",
                                               "--strict", OutputPath};
  std::string VerifyError;
  EXPECT_EQ(runProgram(Verify, VerifyError), 0) << VerifyError;
}

TEST(NeverDSafetyPatchCAPI,
     CryptographicallyInvalidSourceSignatureIsAuthenticationFailure) {
  REQUIRE_RUNTIME_FIXTURE_COMPILER();
  TemporaryFixture Fixture;
  ASSERT_TRUE(Fixture.valid()) << Fixture.Error;
  ASSERT_TRUE(Fixture.compileRuntimeFixture()) << Fixture.Error;
  std::vector<uint8_t> Corrupted = readBytes(Fixture.Input);
  ASSERT_TRUE(corruptPrimaryCodeHash(Corrupted));
  writeBytes(Fixture.Input, Corrupted);
  SessionGuard Session;
  ASSERT_EQ(neverd_session_load(Session.Value, Fixture.Input.string().c_str()),
            1)
      << takeString(neverd_last_error(Session.Value));
  neverd_sanitize_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  neverd_sanitize_result_v1 Result{};
  Result.struct_size = sizeof(Result);

  EXPECT_EQ(neverd_session_sanitize(Session.Value,
                                    Fixture.Output.string().c_str(), &Options,
                                    &Result),
            0);
  EXPECT_EQ(Result.status, NEVERD_SANITIZE_STATUS_AUTHENTICATION_FAILED);
  EXPECT_NE(takeString(neverd_last_error(Session.Value))
                .find("source signature verification failed"),
            std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(Fixture.Output));
  EXPECT_EQ(Fixture.sanitizerTemps(), 0u);
}

TEST(NeverDSafetyPatchCAPI,
     GuardedBundleMemberFailsBeforeInnerMachOSignatureIsChanged) {
  REQUIRE_RUNTIME_FIXTURE_COMPILER();
  TemporaryFixture Fixture;
  ASSERT_TRUE(Fixture.valid()) << Fixture.Error;
  ASSERT_TRUE(Fixture.compileRuntimeFixture()) << Fixture.Error;
  ASSERT_TRUE(Fixture.moveIntoBundle(".ApP")) << Fixture.Error;
  const std::vector<uint8_t> Original = readBytes(Fixture.Input);
  SessionGuard Session;
  ASSERT_EQ(neverd_session_load(Session.Value, Fixture.Input.string().c_str()),
            1)
      << takeString(neverd_last_error(Session.Value));
  neverd_sanitize_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  neverd_sanitize_result_v1 Result{};
  Result.struct_size = sizeof(Result);

  EXPECT_EQ(neverd_session_sanitize(Session.Value,
                                    Fixture.Output.string().c_str(), &Options,
                                    &Result),
            0);
  EXPECT_EQ(Result.status, NEVERD_SANITIZE_STATUS_SIGNATURE_UNSUPPORTED);
  EXPECT_NE(takeString(neverd_last_error(Session.Value)).find("bundle member"),
            std::string::npos);
  EXPECT_EQ(readBytes(Fixture.Input), Original);
  EXPECT_FALSE(std::filesystem::exists(Fixture.Output));
}

TEST(NeverDSafetyPatchCAPI, HardenedMachOSignatureFailsClosed) {
  REQUIRE_RUNTIME_FIXTURE_COMPILER();
  TemporaryFixture Fixture;
  ASSERT_TRUE(Fixture.valid()) << Fixture.Error;
  ASSERT_TRUE(Fixture.compileRuntimeFixture()) << Fixture.Error;
  const std::string InputPath = Fixture.Input.string();
  const std::vector<llvm::StringRef> Sign = {
      "/usr/bin/codesign", "--force", "--sign",       "-",
      "--options",         "runtime", "--identifier", "com.neverd.test",
      "--timestamp=none",  InputPath};
  std::string SignError;
  ASSERT_EQ(runProgram(Sign, SignError), 0) << SignError;
  SessionGuard Session;
  ASSERT_EQ(neverd_session_load(Session.Value, InputPath.c_str()), 1)
      << takeString(neverd_last_error(Session.Value));
  neverd_sanitize_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  neverd_sanitize_result_v1 Result{};
  Result.struct_size = sizeof(Result);

  EXPECT_EQ(neverd_session_sanitize(Session.Value,
                                    Fixture.Output.string().c_str(), &Options,
                                    &Result),
            0);
  EXPECT_EQ(Result.status, NEVERD_SANITIZE_STATUS_SIGNATURE_UNSUPPORTED);
  EXPECT_NE(takeString(neverd_last_error(Session.Value)).find("signature"),
            std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(Fixture.Output));
}

TEST(NeverDSafetyPatchCAPI,
     CodesignFailurePreservesDestinationAndPriorSessionState) {
  REQUIRE_RUNTIME_FIXTURE_COMPILER();
  TemporaryFixture Fixture;
  ASSERT_TRUE(Fixture.valid()) << Fixture.Error;
  ASSERT_TRUE(Fixture.compileRuntimeFixture()) << Fixture.Error;
  const std::vector<uint8_t> DestinationBytes = {'o', 'l', 'd'};
  writeBytes(Fixture.Output, DestinationBytes);
  SessionGuard Session;
  ASSERT_EQ(neverd_session_load(Session.Value, Fixture.Input.string().c_str()),
            1)
      << takeString(neverd_last_error(Session.Value));
  auto *Internal = neverd::sdk::toSession(Session.Value);
  seedPriorSessionResults(*Internal);
  Internal->SanitizeAfterBackendForTesting =
      [](const std::filesystem::path &Candidate) {
        std::error_code EC;
        std::filesystem::remove(Candidate, EC);
        EXPECT_FALSE(EC) << EC.message();
      };
  neverd_sanitize_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  neverd_sanitize_result_v1 Result{};
  Result.struct_size = sizeof(Result);

  EXPECT_EQ(neverd_session_sanitize(Session.Value,
                                    Fixture.Output.string().c_str(), &Options,
                                    &Result),
            0);
  EXPECT_EQ(Result.status, NEVERD_SANITIZE_STATUS_SIGNING_FAILED);
  const std::string LastError = takeString(neverd_last_error(Session.Value));
  EXPECT_NE(LastError.find("codesign"), std::string::npos) << LastError;
  EXPECT_EQ(readBytes(Fixture.Output), DestinationBytes);
  EXPECT_EQ(Fixture.sanitizerTemps(), 0u);
  expectPriorSessionResults(Session.Value);
}
#endif

#ifdef __APPLE__
TEST(NeverDSafetyPatchCAPI,
     GuardedPublicationPreservesExecutableModeButClearsPrivilegeBits) {
  REQUIRE_RUNTIME_FIXTURE_COMPILER();
  TemporaryFixture Fixture;
  ASSERT_TRUE(Fixture.valid()) << Fixture.Error;
  ASSERT_TRUE(Fixture.compileRuntimeFixture()) << Fixture.Error;
  const auto Requested =
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
      std::filesystem::perms::owner_exec | std::filesystem::perms::group_read |
      std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
      std::filesystem::perms::others_exec | std::filesystem::perms::set_uid |
      std::filesystem::perms::set_gid;
  std::error_code EC;
  std::filesystem::permissions(Fixture.Input, Requested,
                               std::filesystem::perm_options::replace, EC);
  ASSERT_FALSE(EC) << EC.message();
  const auto InputMode =
      std::filesystem::status(Fixture.Input, EC).permissions();
  ASSERT_FALSE(EC) << EC.message();
  if ((InputMode &
       (std::filesystem::perms::set_uid | std::filesystem::perms::set_gid)) ==
      std::filesystem::perms::none)
    GTEST_SKIP() << "host filesystem does not retain setuid/setgid bits";

  SessionGuard Session;
  ASSERT_EQ(neverd_session_load(Session.Value, Fixture.Input.string().c_str()),
            1)
      << takeString(neverd_last_error(Session.Value));
  neverd_sanitize_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  neverd_sanitize_result_v1 Result{};
  Result.struct_size = sizeof(Result);
  ASSERT_EQ(neverd_session_sanitize(Session.Value,
                                    Fixture.Output.string().c_str(), &Options,
                                    &Result),
            1)
      << takeString(neverd_last_error(Session.Value));

  const auto OutputMode =
      std::filesystem::status(Fixture.Output, EC).permissions();
  ASSERT_FALSE(EC) << EC.message();
  EXPECT_NE(OutputMode & std::filesystem::perms::owner_exec,
            std::filesystem::perms::none);
  EXPECT_EQ(OutputMode & (std::filesystem::perms::set_uid |
                          std::filesystem::perms::set_gid),
            std::filesystem::perms::none);
}
#endif

#ifdef __APPLE__
TEST(NeverDSafetyPatchCAPI,
     CandidateSwapBeforeReloadFailsWithoutPublishingOrStateCommit) {
  REQUIRE_RUNTIME_FIXTURE_COMPILER();
  TemporaryFixture Fixture;
  ASSERT_TRUE(Fixture.valid()) << Fixture.Error;
  ASSERT_TRUE(Fixture.compileRuntimeFixture()) << Fixture.Error;
  const std::vector<uint8_t> Original = readBytes(Fixture.Input);
  ASSERT_FALSE(Original.empty());
  const std::vector<uint8_t> DestinationBytes = {'o', 'l', 'd'};
  writeBytes(Fixture.Output, DestinationBytes);

  SessionGuard Session;
  ASSERT_EQ(neverd_session_load(Session.Value, Fixture.Input.string().c_str()),
            1)
      << takeString(neverd_last_error(Session.Value));
  auto *Internal = neverd::sdk::toSession(Session.Value);
  seedPriorSessionResults(*Internal);
  Internal->SanitizeBeforeReloadForTesting =
      [&Original](const std::filesystem::path &Candidate) {
        writeBytes(Candidate, Original);
      };
  neverd_sanitize_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  neverd_sanitize_result_v1 Result{};
  Result.struct_size = sizeof(Result);

  EXPECT_EQ(neverd_session_sanitize(Session.Value,
                                    Fixture.Output.string().c_str(), &Options,
                                    &Result),
            0);
  EXPECT_EQ(Result.status, NEVERD_SANITIZE_STATUS_AUTHENTICATION_FAILED);
  EXPECT_NE(takeString(neverd_last_error(Session.Value)).find("changed"),
            std::string::npos);
  EXPECT_EQ(readBytes(Fixture.Output), DestinationBytes);
  EXPECT_EQ(Fixture.sanitizerTemps(), 0u);
  expectPriorSessionResults(Session.Value);
}

TEST(NeverDSafetyPatchCAPI,
     BackendPathReplacementIsCapturedIntoFreshPublicationTemp) {
  REQUIRE_RUNTIME_FIXTURE_COMPILER();
  TemporaryFixture Fixture;
  ASSERT_TRUE(Fixture.valid()) << Fixture.Error;
  ASSERT_TRUE(Fixture.compileRuntimeFixture()) << Fixture.Error;
  SessionGuard Session;
  ASSERT_EQ(neverd_session_load(Session.Value, Fixture.Input.string().c_str()),
            1)
      << takeString(neverd_last_error(Session.Value));
  auto *Internal = neverd::sdk::toSession(Session.Value);
  bool HookRan = false;
  std::error_code ReplacementError;
  std::vector<uint8_t> AuthenticatedBytes;
  Internal->SanitizeAfterBackendForTesting =
      [&](const std::filesystem::path &Candidate) {
        HookRan = true;
        AuthenticatedBytes = readBytes(Candidate);
        std::error_code PermissionError;
        const std::filesystem::perms CandidatePermissions =
            std::filesystem::status(Candidate, PermissionError).permissions();
        if (PermissionError) {
          ReplacementError = PermissionError;
          return;
        }
        std::filesystem::path Replacement = Candidate;
        Replacement += ".replacement";
        writeBytes(Replacement, AuthenticatedBytes);
        std::filesystem::permissions(Replacement, CandidatePermissions,
                                     std::filesystem::perm_options::replace,
                                     PermissionError);
        if (PermissionError) {
          ReplacementError = PermissionError;
          return;
        }
        ReplacementError =
            llvm::sys::fs::rename(Replacement.string(), Candidate.string());
      };
  neverd_sanitize_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  neverd_sanitize_result_v1 Result{};
  Result.struct_size = sizeof(Result);

  ASSERT_EQ(neverd_session_sanitize(Session.Value,
                                    Fixture.Output.string().c_str(), &Options,
                                    &Result),
            1)
      << neverd_sanitize_status_name(Result.status) << ": "
      << takeString(neverd_last_error(Session.Value));
  EXPECT_TRUE(HookRan);
  EXPECT_FALSE(ReplacementError) << ReplacementError.message();
  EXPECT_FALSE(AuthenticatedBytes.empty());
#ifndef __APPLE__
  EXPECT_EQ(readBytes(Fixture.Output), AuthenticatedBytes);
#else
  const std::string OutputPath = Fixture.Output.string();
  const std::vector<llvm::StringRef> Verify = {"/usr/bin/codesign", "--verify",
                                               "--strict", OutputPath};
  std::string VerifyError;
  EXPECT_EQ(runProgram(Verify, VerifyError), 0) << VerifyError;
#endif
  EXPECT_EQ(Result.status, NEVERD_SANITIZE_STATUS_OK);
  EXPECT_EQ(Result.publication_outcome,
            NEVERD_SANITIZE_PUBLICATION_OUTCOME_PUBLISHED);
  EXPECT_EQ(Result.publication_receipt_complete, 1u);
  EXPECT_EQ(Result.guarded_sites, 1u);
  EXPECT_EQ(Fixture.sanitizerTemps(), 0u);
}

TEST(NeverDSafetyPatchCAPI,
     CandidateSwapBeforePublishFailsWithoutPublishingOrStateCommit) {
  REQUIRE_RUNTIME_FIXTURE_COMPILER();
  TemporaryFixture Fixture;
  ASSERT_TRUE(Fixture.valid()) << Fixture.Error;
  ASSERT_TRUE(Fixture.compileRuntimeFixture()) << Fixture.Error;
  const std::vector<uint8_t> Original = readBytes(Fixture.Input);
  ASSERT_FALSE(Original.empty());
  const std::vector<uint8_t> DestinationBytes = {'o', 'l', 'd'};
  writeBytes(Fixture.Output, DestinationBytes);

  SessionGuard Session;
  ASSERT_EQ(neverd_session_load(Session.Value, Fixture.Input.string().c_str()),
            1)
      << takeString(neverd_last_error(Session.Value));
  auto *Internal = neverd::sdk::toSession(Session.Value);
  seedPriorSessionResults(*Internal);
  Internal->SanitizeBeforePublishForTesting =
      [&Original](const std::filesystem::path &Candidate) {
        writeBytes(Candidate, Original);
      };
  neverd_sanitize_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  neverd_sanitize_result_v1 Result{};
  Result.struct_size = sizeof(Result);

  EXPECT_EQ(neverd_session_sanitize(Session.Value,
                                    Fixture.Output.string().c_str(), &Options,
                                    &Result),
            0);
  EXPECT_EQ(Result.status, NEVERD_SANITIZE_STATUS_AUTHENTICATION_FAILED);
  EXPECT_NE(takeString(neverd_last_error(Session.Value)).find("changed"),
            std::string::npos);
  EXPECT_EQ(readBytes(Fixture.Output), DestinationBytes);
  EXPECT_EQ(Fixture.sanitizerTemps(), 0u);
  expectPriorSessionResults(Session.Value);
}

TEST(NeverDSafetyPatchCAPI,
     ConcurrentDestinationChangeIsNotOverwrittenOrCommitted) {
  REQUIRE_RUNTIME_FIXTURE_COMPILER();
  TemporaryFixture Fixture;
  ASSERT_TRUE(Fixture.valid()) << Fixture.Error;
  ASSERT_TRUE(Fixture.compileRuntimeFixture()) << Fixture.Error;
  const std::vector<uint8_t> ConcurrentBytes = {'n', 'e', 'w'};

  SessionGuard Session;
  ASSERT_EQ(neverd_session_load(Session.Value, Fixture.Input.string().c_str()),
            1)
      << takeString(neverd_last_error(Session.Value));
  auto *Internal = neverd::sdk::toSession(Session.Value);
  seedPriorSessionResults(*Internal);
  Internal->SanitizeBeforePublishForTesting =
      [&Fixture, &ConcurrentBytes](const std::filesystem::path &) {
        writeBytes(Fixture.Output, ConcurrentBytes);
      };
  neverd_sanitize_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  neverd_sanitize_result_v1 Result{};
  Result.struct_size = sizeof(Result);

  EXPECT_EQ(neverd_session_sanitize(Session.Value,
                                    Fixture.Output.string().c_str(), &Options,
                                    &Result),
            0);
  EXPECT_EQ(Result.status, NEVERD_SANITIZE_STATUS_PUBLISH_FAILED);
  EXPECT_EQ(Result.publication_outcome,
            NEVERD_SANITIZE_PUBLICATION_OUTCOME_NOT_ATTEMPTED);
  EXPECT_NE(
      takeString(neverd_last_error(Session.Value)).find("replacement CAS"),
      std::string::npos);
  EXPECT_EQ(readBytes(Fixture.Output), ConcurrentBytes);
  EXPECT_EQ(Fixture.sanitizerTemps(), 0u);
  expectPriorSessionResults(Session.Value);
}

TEST(NeverDSafetyPatchCAPI,
     GuardedPlanRejectsLoadedSourceAsDestinationWithoutStateCommit) {
  REQUIRE_RUNTIME_FIXTURE_COMPILER();
  TemporaryFixture Fixture;
  ASSERT_TRUE(Fixture.valid()) << Fixture.Error;
  ASSERT_TRUE(Fixture.compileRuntimeFixture()) << Fixture.Error;
  const std::vector<uint8_t> Original = readBytes(Fixture.Input);
  ASSERT_FALSE(Original.empty());

  SessionGuard Session;
  ASSERT_EQ(neverd_session_load(Session.Value, Fixture.Input.string().c_str()),
            1)
      << takeString(neverd_last_error(Session.Value));
  auto *Internal = neverd::sdk::toSession(Session.Value);
  seedPriorSessionResults(*Internal);
  neverd_sanitize_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  neverd_sanitize_result_v1 Result{};
  Result.struct_size = sizeof(Result);

  EXPECT_EQ(neverd_session_sanitize(Session.Value,
                                    Fixture.Input.string().c_str(), &Options,
                                    &Result),
            0);
  EXPECT_EQ(Result.status, NEVERD_SANITIZE_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(Result.plan_version, 1u);
  EXPECT_EQ(Result.findings, 1u);
  EXPECT_EQ(Result.guarded_sites, 1u);
  EXPECT_NE(
      takeString(neverd_last_error(Session.Value)).find("must not replace"),
      std::string::npos);
  EXPECT_EQ(readBytes(Fixture.Input), Original);
  EXPECT_EQ(Fixture.sanitizerTemps(), 0u);
  expectPriorSessionResults(Session.Value);
}

TEST(NeverDSafetyPatchCAPI,
     ExceptionBeforePublicationFailsClosedAndPreservesSessionState) {
  REQUIRE_RUNTIME_FIXTURE_COMPILER();
  TemporaryFixture Fixture;
  ASSERT_TRUE(Fixture.valid()) << Fixture.Error;
  ASSERT_TRUE(Fixture.compileRuntimeFixture()) << Fixture.Error;
  const std::vector<uint8_t> DestinationBytes = {'o', 'l', 'd'};
  writeBytes(Fixture.Output, DestinationBytes);

  SessionGuard Session;
  ASSERT_EQ(neverd_session_load(Session.Value, Fixture.Input.string().c_str()),
            1)
      << takeString(neverd_last_error(Session.Value));
  auto *Internal = neverd::sdk::toSession(Session.Value);
  seedPriorSessionResults(*Internal);
  Internal->SanitizeBeforePublishForTesting =
      [](const std::filesystem::path &) {
        throw std::runtime_error("injected publication exception");
      };
  neverd_sanitize_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  neverd_sanitize_result_v1 Result{};
  Result.struct_size = sizeof(Result);

  EXPECT_EQ(neverd_session_sanitize(Session.Value,
                                    Fixture.Output.string().c_str(), &Options,
                                    &Result),
            0);
  EXPECT_EQ(Result.ok, 0);
  EXPECT_EQ(Result.status, NEVERD_SANITIZE_STATUS_PIPELINE_FAILED);
  EXPECT_EQ(Result.publication_outcome,
            NEVERD_SANITIZE_PUBLICATION_OUTCOME_NOT_ATTEMPTED);
  EXPECT_EQ(Result.publication_receipt_version,
            NEVERD_SANITIZE_PUBLICATION_ABI_VERSION);
  EXPECT_EQ(Result.publication_receipt_complete, 0u);
  EXPECT_EQ(Result.publication_namespace_disposition,
            NEVERD_SANITIZE_PUBLICATION_NAMESPACE_NONE);
  EXPECT_EQ(Result.publication_guarantee_flags, 0u);
  EXPECT_EQ(Result.publication_operand_binding,
            NEVERD_SANITIZE_PUBLICATION_OPERAND_BINDING_NONE);
  EXPECT_NE(takeString(neverd_last_error(Session.Value))
                .find("injected publication exception"),
            std::string::npos);
  EXPECT_EQ(readBytes(Fixture.Output), DestinationBytes);
  EXPECT_EQ(Fixture.sanitizerTemps(), 0u);
  expectPriorSessionResults(Session.Value);
}
#endif
