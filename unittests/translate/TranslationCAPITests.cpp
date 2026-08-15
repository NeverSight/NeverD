//===- TranslationCAPITests.cpp - Public cross-architecture C ABI -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPI.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBufferRef.h"
#include "llvm/TargetParser/Triple.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace {

static_assert(std::is_same_v<neverd_translate_object_format_t, std::uint32_t>);
static_assert(std::is_same_v<neverd_translate_error_code_t, std::uint32_t>);
static_assert(std::is_same_v<neverd_translate_semantic_stop_t, std::uint32_t>);
static_assert(std::is_same_v<neverd_translate_proof_status_t, std::uint32_t>);

constexpr uint64_t EntryPC = 0x401000;
constexpr uint64_t Generation = 37;
constexpr std::array<unsigned char, 8> GuestBytes = {0x48, 0x89, 0xf8, 0x48,
                                                     0x83, 0xc0, 0x01, 0xc3};

neverd_translate_object_request_v1
request(neverd_translate_object_format_t Format,
        llvm::ArrayRef<unsigned char> Bytes = GuestBytes) {
  neverd_translate_object_request_v1 Request{};
  Request.struct_size = sizeof(Request);
  Request.guest_bytes = Bytes.data();
  Request.guest_bytes_size = Bytes.size();
  Request.entry_pc = EntryPC;
  Request.executable_generation = Generation;
  Request.object_format = Format;
  return Request;
}

std::unique_ptr<llvm::object::ObjectFile>
parseObject(const neverd_translate_object_result_v1 &Result) {
  const llvm::StringRef Contents(
      reinterpret_cast<const char *>(Result.object_bytes), Result.object_size);
  llvm::Expected<std::unique_ptr<llvm::object::ObjectFile>> ObjectOrErr =
      llvm::object::ObjectFile::createObjectFile(
          llvm::MemoryBufferRef(Contents, "neverd-translation-c-api"));
  if (!ObjectOrErr) {
    ADD_FAILURE() << llvm::toString(ObjectOrErr.takeError());
    return nullptr;
  }
  return std::move(*ObjectOrErr);
}

void expectCommonSuccess(const neverd_translate_object_result_v1 &Result,
                         uint64_t ExpectedInstructionCount = 3,
                         uint64_t ExpectedGuestByteCount = GuestBytes.size()) {
  ASSERT_EQ(Result.ok, 1) << (Result.error_message ? Result.error_message : "");
  EXPECT_EQ(Result.error_code, NEVERD_TRANSLATE_ERROR_NONE);
  EXPECT_EQ(Result.error_message, nullptr);
  ASSERT_NE(Result.object_bytes, nullptr);
  EXPECT_GT(Result.object_size, 0u);
  EXPECT_EQ(Result.guest_entry_pc, EntryPC);
  EXPECT_EQ(Result.guest_instruction_count, ExpectedInstructionCount);
  EXPECT_EQ(Result.guest_byte_count, ExpectedGuestByteCount);
  EXPECT_EQ(Result.executable_generation, Generation);
  ASSERT_NE(Result.block_ir_symbol, nullptr);
  EXPECT_STREQ(Result.block_ir_symbol, "nvd_x86_64_block_0000000000401000");
  ASSERT_NE(Result.host_target_identity, nullptr);
  EXPECT_TRUE(llvm::StringRef(Result.host_target_identity)
                  .starts_with("neverd.host-target.v1\n"));
  ASSERT_NE(Result.runtime_registry_identity, nullptr);
  EXPECT_TRUE(llvm::StringRef(Result.runtime_registry_identity)
                  .starts_with("nvd-runtime-symbol-registry-v1-sha256:"));
  ASSERT_NE(Result.request_cache_key, nullptr);
  EXPECT_TRUE(llvm::StringRef(Result.request_cache_key)
                  .starts_with("neverd.translation-object-request.v1.sha256:"));
  ASSERT_NE(Result.artifact_cache_key, nullptr);
  EXPECT_TRUE(
      llvm::StringRef(Result.artifact_cache_key)
          .starts_with("neverd.translation-object-artifact.v1.sha256:"));
  ASSERT_NE(Result.translation_cache_identity, nullptr);
  EXPECT_TRUE(llvm::StringRef(Result.translation_cache_identity)
                  .starts_with("neverd.translation-object-wrapper.v1.sha256:"));
  EXPECT_GT(Result.semantic_function_pass_invocations, 0u);
  EXPECT_EQ(Result.semantic_stop, NEVERD_TRANSLATE_SEMANTIC_STABLE);
  EXPECT_GE(Result.semantic_proof, NEVERD_TRANSLATE_PROOF_NOT_RUN);
  EXPECT_LE(Result.semantic_proof, NEVERD_TRANSLATE_PROOF_INVALID);
  EXPECT_EQ(Result.llvm_optimization_pipeline_ran, 1);
  EXPECT_EQ(Result.object_cache_identity_version, 1u);
  EXPECT_EQ(Result.object_pipeline_schema_version, 3u);
}

TEST(NeverDTranslationCAPI, EmitsOwnedAuditedAArch64ELFObject) {
  std::array<unsigned char, GuestBytes.size()> Borrowed = GuestBytes;
  neverd_translate_object_request_v1 Request =
      request(NEVERD_TRANSLATE_OBJECT_FORMAT_ELF, Borrowed);
  neverd_translate_object_result_v1 Result{};
  Result.struct_size = sizeof(Result);

  ASSERT_EQ(
      neverd_translate_x86_64_block_to_aarch64_object_v1(&Request, &Result), 0);
  expectCommonSuccess(Result);
  EXPECT_EQ(Result.object_format, NEVERD_TRANSLATE_OBJECT_FORMAT_ELF);
  ASSERT_NE(Result.host_triple, nullptr);
  EXPECT_STREQ(Result.host_triple, "aarch64-unknown-linux-gnu");
  ASSERT_NE(Result.host_cpu, nullptr);
  EXPECT_STREQ(Result.host_cpu, "");
  ASSERT_NE(Result.block_object_symbol, nullptr);
  EXPECT_STREQ(Result.block_object_symbol, Result.block_ir_symbol);
  EXPECT_EQ(Borrowed, GuestBytes);

  std::unique_ptr<llvm::object::ObjectFile> Object = parseObject(Result);
  ASSERT_NE(Object, nullptr);
  EXPECT_TRUE(Object->isRelocatableObject());
  EXPECT_TRUE(Object->isELF());
  EXPECT_EQ(Object->getArch(), llvm::Triple::aarch64);

  neverd_translate_object_result_dispose(&Result);
  neverd_translate_object_result_dispose(&Result);
  EXPECT_EQ(Result.object_bytes, nullptr);
  EXPECT_EQ(Result.block_ir_symbol, nullptr);
  EXPECT_EQ(Result.translation_cache_identity, nullptr);
  EXPECT_EQ(Result.ok, 0);
}

TEST(NeverDTranslationCAPI, EmitsOwnedAuditedAArch64MachOObject) {
  neverd_translate_object_request_v1 Request =
      request(NEVERD_TRANSLATE_OBJECT_FORMAT_MACHO);
  neverd_translate_object_result_v1 Result{};
  Result.struct_size = sizeof(Result);

  ASSERT_EQ(
      neverd_translate_x86_64_block_to_aarch64_object_v1(&Request, &Result), 0);
  expectCommonSuccess(Result);
  EXPECT_EQ(Result.object_format, NEVERD_TRANSLATE_OBJECT_FORMAT_MACHO);
  ASSERT_NE(Result.host_triple, nullptr);
  EXPECT_STREQ(Result.host_triple, "aarch64-apple-macosx");
  ASSERT_NE(Result.block_object_symbol, nullptr);
  EXPECT_STREQ(Result.block_object_symbol,
               "_nvd_x86_64_block_0000000000401000");

  std::unique_ptr<llvm::object::ObjectFile> Object = parseObject(Result);
  ASSERT_NE(Object, nullptr);
  EXPECT_TRUE(Object->isRelocatableObject());
  EXPECT_TRUE(Object->isMachO());
  EXPECT_EQ(Object->getArch(), llvm::Triple::aarch64);
  neverd_translate_object_result_dispose(&Result);
}

TEST(NeverDTranslationCAPI, AcceptsPublishedScalarRegisterSubset) {
  constexpr std::array<unsigned char, 10> ScalarBlock = {
      0x48, 0x89, 0xd8, 0x48, 0x83, 0xe8, 0x02, 0xc2, 0x10, 0x00};
  neverd_translate_object_request_v1 Request =
      request(NEVERD_TRANSLATE_OBJECT_FORMAT_ELF, ScalarBlock);
  neverd_translate_object_result_v1 Result{};
  Result.struct_size = sizeof(Result);

  ASSERT_EQ(
      neverd_translate_x86_64_block_to_aarch64_object_v1(&Request, &Result), 0);
  expectCommonSuccess(Result, /*ExpectedInstructionCount=*/3,
                      /*ExpectedGuestByteCount=*/ScalarBlock.size());
  std::unique_ptr<llvm::object::ObjectFile> Object = parseObject(Result);
  ASSERT_NE(Object, nullptr);
  EXPECT_TRUE(Object->isELF());
  EXPECT_EQ(Object->getArch(), llvm::Triple::aarch64);
  neverd_translate_object_result_dispose(&Result);
}

void expectControlTransferObject(llvm::ArrayRef<unsigned char> GuestControl) {
  neverd_translate_object_request_v1 Request =
      request(NEVERD_TRANSLATE_OBJECT_FORMAT_ELF, GuestControl);
  neverd_translate_object_result_v1 Result{};
  Result.struct_size = sizeof(Result);

  ASSERT_EQ(
      neverd_translate_x86_64_block_to_aarch64_object_v1(&Request, &Result), 0);
  expectCommonSuccess(Result, /*ExpectedInstructionCount=*/1,
                      /*ExpectedGuestByteCount=*/GuestControl.size());
  std::unique_ptr<llvm::object::ObjectFile> Object = parseObject(Result);
  ASSERT_NE(Object, nullptr);
  EXPECT_TRUE(Object->isELF());
  EXPECT_EQ(Object->getArch(), llvm::Triple::aarch64);
  neverd_translate_object_result_dispose(&Result);
}

TEST(NeverDTranslationCAPI, EmitsOwnedObjectForCanonicalShortDirectJump) {
  constexpr std::array<unsigned char, 2> DirectJump = {0xeb, 0xfe};
  expectControlTransferObject(DirectJump);
}

TEST(NeverDTranslationCAPI, EmitsOwnedObjectForCanonicalNearDirectJump) {
  constexpr std::array<unsigned char, 5> DirectJump = {0xe9, 0xfb, 0xff, 0xff,
                                                       0xff};
  expectControlTransferObject(DirectJump);
}

TEST(NeverDTranslationCAPI,
     EmitsOwnedObjectsForCanonicalZeroFlagConditionalBranches) {
  constexpr std::array<unsigned char, 2> ShortEqual = {0x74, 0xfe};
  constexpr std::array<unsigned char, 6> NearNotEqual = {0x0f, 0x85, 0xfa,
                                                         0xff, 0xff, 0xff};
  expectControlTransferObject(ShortEqual);
  expectControlTransferObject(NearNotEqual);
}

TEST(NeverDTranslationCAPI,
     EmitsOwnedObjectsForCanonicalSingleFlagConditionalBranches) {
  constexpr std::array<unsigned char, 2> ShortOverflow = {0x70, 0xfe};
  constexpr std::array<unsigned char, 6> NearNotParity = {0x0f, 0x8b, 0xfa,
                                                          0xff, 0xff, 0xff};
  expectControlTransferObject(ShortOverflow);
  expectControlTransferObject(NearNotParity);
}

TEST(NeverDTranslationCAPI,
     EmitsOwnedObjectsForCanonicalMultiFlagConditionalBranches) {
  constexpr std::array<unsigned char, 2> ShortBelowOrEqual = {0x76, 0xfe};
  constexpr std::array<unsigned char, 6> NearAbove = {0x0f, 0x87, 0xfa,
                                                      0xff, 0xff, 0xff};
  constexpr std::array<unsigned char, 2> ShortLess = {0x7c, 0xfe};
  constexpr std::array<unsigned char, 6> NearGreaterOrEqual = {
      0x0f, 0x8d, 0xfa, 0xff, 0xff, 0xff};
  constexpr std::array<unsigned char, 2> ShortLessOrEqual = {0x7e, 0xfe};
  constexpr std::array<unsigned char, 6> NearGreater = {0x0f, 0x8f, 0xfa,
                                                        0xff, 0xff, 0xff};
  expectControlTransferObject(ShortBelowOrEqual);
  expectControlTransferObject(NearAbove);
  expectControlTransferObject(ShortLess);
  expectControlTransferObject(NearGreaterOrEqual);
  expectControlTransferObject(ShortLessOrEqual);
  expectControlTransferObject(NearGreater);
}

TEST(NeverDTranslationCAPI,
     EmitsOwnedObjectsForEveryPublishedCompareAndTestEncoding) {
  struct PublishedEncoding {
    const char *Name;
    std::vector<unsigned char> Bytes;
  };
  const std::array<PublishedEncoding, 8> Encodings = {{
      {"cmp-39-register", {0x48, 0x39, 0xd8, 0x74, 0xfb}},
      {"cmp-3b-register", {0x48, 0x3b, 0xc3, 0x74, 0xfb}},
      {"cmp-81-negative-imm32",
       {0x48, 0x81, 0xf8, 0xfe, 0xff, 0xff, 0xff, 0x74, 0xf7}},
      {"cmp-83-negative-imm8", {0x48, 0x83, 0xf8, 0xfe, 0x74, 0xfa}},
      {"cmp-3d-negative-imm32",
       {0x48, 0x3d, 0xfe, 0xff, 0xff, 0xff, 0x74, 0xf8}},
      {"test-85-register", {0x48, 0x85, 0xc0, 0x75, 0xfb}},
      {"test-f7-negative-imm32",
       {0x48, 0xf7, 0xc0, 0xfe, 0xff, 0xff, 0xff, 0x75, 0xf7}},
      {"test-a9-negative-imm32",
       {0x48, 0xa9, 0xfe, 0xff, 0xff, 0xff, 0x75, 0xf8}},
  }};

  for (const PublishedEncoding &Encoding : Encodings) {
    SCOPED_TRACE(Encoding.Name);
    const llvm::ArrayRef<unsigned char> GuestBlock(Encoding.Bytes);
    neverd_translate_object_request_v1 Request =
        request(NEVERD_TRANSLATE_OBJECT_FORMAT_ELF, GuestBlock);
    neverd_translate_object_result_v1 Result{};
    Result.struct_size = sizeof(Result);

    ASSERT_EQ(
        neverd_translate_x86_64_block_to_aarch64_object_v1(&Request, &Result),
        0);
    expectCommonSuccess(Result, /*ExpectedInstructionCount=*/2,
                        /*ExpectedGuestByteCount=*/GuestBlock.size());
    std::unique_ptr<llvm::object::ObjectFile> Object = parseObject(Result);
    ASSERT_NE(Object, nullptr);
    EXPECT_TRUE(Object->isRelocatableObject());
    EXPECT_TRUE(Object->isELF());
    EXPECT_EQ(Object->getArch(), llvm::Triple::aarch64);
    neverd_translate_object_result_dispose(&Result);
  }
}

TEST(NeverDTranslationCAPI, MapsTypedRequestFailuresAndOwnsTheDiagnostic) {
  constexpr std::array<unsigned char, 2> Unsupported = {0x90, 0xc3};
  neverd_translate_object_request_v1 Request =
      request(NEVERD_TRANSLATE_OBJECT_FORMAT_ELF, Unsupported);
  neverd_translate_object_result_v1 Result{};
  Result.struct_size = sizeof(Result);

  ASSERT_EQ(
      neverd_translate_x86_64_block_to_aarch64_object_v1(&Request, &Result), 0);
  EXPECT_EQ(Result.ok, 0);
  EXPECT_EQ(Result.error_code, NEVERD_TRANSLATE_ERROR_BLOCK_LOWERING_FAILED);
  EXPECT_STREQ(neverd_translate_error_code_name(Result.error_code),
               "block-lowering-failed");
  ASSERT_NE(Result.error_message, nullptr);
  EXPECT_TRUE(llvm::StringRef(Result.error_message)
                  .contains("translation object request"));
  EXPECT_EQ(Result.object_bytes, nullptr);
  neverd_translate_object_result_dispose(&Result);
  neverd_translate_object_result_dispose(&Result);
}

TEST(NeverDTranslationCAPI, RejectsTrailingBytesAfterTheSingleBlock) {
  constexpr std::array<unsigned char, 9> BlockWithTrailingByte = {
      0x48, 0x89, 0xf8, 0x48, 0x83, 0xc0, 0x01, 0xc3, 0x90};
  neverd_translate_object_request_v1 Request =
      request(NEVERD_TRANSLATE_OBJECT_FORMAT_ELF, BlockWithTrailingByte);
  neverd_translate_object_result_v1 Result{};
  Result.struct_size = sizeof(Result);

  ASSERT_EQ(
      neverd_translate_x86_64_block_to_aarch64_object_v1(&Request, &Result), 0);
  EXPECT_EQ(Result.ok, 0);
  EXPECT_EQ(Result.error_code, NEVERD_TRANSLATE_ERROR_INVALID_ARGUMENT);
  ASSERT_NE(Result.error_message, nullptr);
  EXPECT_TRUE(llvm::StringRef(Result.error_message).contains("trailing bytes"));
  EXPECT_EQ(Result.object_bytes, nullptr);
  neverd_translate_object_result_dispose(&Result);
}

TEST(NeverDTranslationCAPI, RejectsInvalidAndShortBorrowedRequests) {
  neverd_translate_object_result_v1 Result{};
  Result.struct_size = sizeof(Result);
  ASSERT_EQ(
      neverd_translate_x86_64_block_to_aarch64_object_v1(nullptr, &Result), 0);
  EXPECT_EQ(Result.error_code, NEVERD_TRANSLATE_ERROR_INVALID_ARGUMENT);
  neverd_translate_object_result_dispose(&Result);

  neverd_translate_object_request_v1 Request =
      request(NEVERD_TRANSLATE_OBJECT_FORMAT_ELF);
  Request.struct_size = offsetof(neverd_translate_object_request_v1, reserved);
  Result.struct_size = sizeof(Result);
  ASSERT_EQ(
      neverd_translate_x86_64_block_to_aarch64_object_v1(&Request, &Result), 0);
  EXPECT_EQ(Result.error_code, NEVERD_TRANSLATE_ERROR_INVALID_ARGUMENT);
  neverd_translate_object_result_dispose(&Result);

  Request = request(static_cast<neverd_translate_object_format_t>(255));
  Result.struct_size = sizeof(Result);
  ASSERT_EQ(
      neverd_translate_x86_64_block_to_aarch64_object_v1(&Request, &Result), 0);
  EXPECT_EQ(Result.error_code, NEVERD_TRANSLATE_ERROR_INVALID_ARGUMENT);
  neverd_translate_object_result_dispose(&Result);

  Request = request(NEVERD_TRANSLATE_OBJECT_FORMAT_ELF);
  Request.reserved = 1;
  Result.struct_size = sizeof(Result);
  ASSERT_EQ(
      neverd_translate_x86_64_block_to_aarch64_object_v1(&Request, &Result), 0);
  EXPECT_EQ(Result.error_code, NEVERD_TRANSLATE_ERROR_INVALID_ARGUMENT);
  ASSERT_NE(Result.error_message, nullptr);
  EXPECT_TRUE(llvm::StringRef(Result.error_message).contains("reserved"));
  neverd_translate_object_result_dispose(&Result);
}

TEST(NeverDTranslationCAPI, HonorsAnOlderResultSizeWithoutTouchingCanary) {
  struct ShortResult {
    size_t struct_size;
    int ok;
    neverd_translate_error_code_t error_code;
    const char *error_message;
    const unsigned char *object_bytes;
    size_t object_size;
    uint32_t canary;
  } Short{offsetof(ShortResult, canary),
          -1,
          NEVERD_TRANSLATE_ERROR_NONE,
          nullptr,
          nullptr,
          0,
          0x7472616eu};

  auto *PublicResult =
      reinterpret_cast<neverd_translate_object_result_v1 *>(&Short);
  ASSERT_EQ(
      neverd_translate_x86_64_block_to_aarch64_object_v1(nullptr, PublicResult),
      0);
  EXPECT_EQ(Short.ok, 0);
  EXPECT_EQ(Short.error_code, NEVERD_TRANSLATE_ERROR_INVALID_ARGUMENT);
  EXPECT_NE(Short.error_message, nullptr);
  EXPECT_EQ(Short.canary, 0x7472616eu);
  neverd_translate_object_result_dispose(PublicResult);
  EXPECT_EQ(Short.error_message, nullptr);
  EXPECT_EQ(Short.canary, 0x7472616eu);
}

TEST(NeverDTranslationCAPI,
     RejectsResultPrefixThatCannotCarrySuccessfulObject) {
  neverd_translate_object_request_v1 Request =
      request(NEVERD_TRANSLATE_OBJECT_FORMAT_ELF);
  struct TooShort {
    size_t struct_size;
    int ok;
    neverd_translate_error_code_t error_code;
    uint32_t canary;
  } Result{offsetof(TooShort, canary), -1, NEVERD_TRANSLATE_ERROR_NONE,
           0x7472616eu};

  EXPECT_EQ(neverd_translate_x86_64_block_to_aarch64_object_v1(
                &Request,
                reinterpret_cast<neverd_translate_object_result_v1 *>(&Result)),
            1);
  EXPECT_EQ(Result.ok, -1);
  EXPECT_EQ(Result.error_code, NEVERD_TRANSLATE_ERROR_NONE);
  EXPECT_EQ(Result.canary, 0x7472616eu);
}

TEST(NeverDTranslationCAPI, EmitsOwnedObjectIntoTheMandatoryResultPrefix) {
  neverd_translate_object_request_v1 Request =
      request(NEVERD_TRANSLATE_OBJECT_FORMAT_ELF);
  struct MinimalResult {
    size_t struct_size;
    int ok;
    neverd_translate_error_code_t error_code;
    const char *error_message;
    const unsigned char *object_bytes;
    size_t object_size;
    uint32_t canary;
  } Result{offsetof(MinimalResult, canary),
           -1,
           NEVERD_TRANSLATE_ERROR_INTERNAL_FAILURE,
           nullptr,
           nullptr,
           0,
           0x7472616eu};

  auto *PublicResult =
      reinterpret_cast<neverd_translate_object_result_v1 *>(&Result);
  ASSERT_EQ(neverd_translate_x86_64_block_to_aarch64_object_v1(&Request,
                                                               PublicResult),
            0);
  ASSERT_EQ(Result.ok, 1) << (Result.error_message ? Result.error_message : "");
  EXPECT_EQ(Result.error_code, NEVERD_TRANSLATE_ERROR_NONE);
  EXPECT_EQ(Result.error_message, nullptr);
  EXPECT_NE(Result.object_bytes, nullptr);
  EXPECT_GT(Result.object_size, 0u);
  EXPECT_EQ(Result.canary, 0x7472616eu);

  neverd_translate_object_result_dispose(PublicResult);
  EXPECT_EQ(Result.object_bytes, nullptr);
  EXPECT_EQ(Result.object_size, 0u);
  EXPECT_EQ(Result.canary, 0x7472616eu);
  EXPECT_EQ(Result.struct_size, offsetof(MinimalResult, canary));
}

TEST(NeverDTranslationCAPI, RejectsAnUnusableResultBuffer) {
  EXPECT_EQ(
      neverd_translate_x86_64_block_to_aarch64_object_v1(nullptr, nullptr), 1);
  struct TooShort {
    size_t struct_size;
    int ok;
  } Result{sizeof(Result), -1};
  EXPECT_EQ(neverd_translate_x86_64_block_to_aarch64_object_v1(
                nullptr,
                reinterpret_cast<neverd_translate_object_result_v1 *>(&Result)),
            1);
  EXPECT_EQ(Result.ok, -1);
}

} // namespace
