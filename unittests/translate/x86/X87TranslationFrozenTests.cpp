//===- X87TranslationFrozenTests.cpp - Freeze actual native x87 blocks ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Retain the exact objects passed to JITLink and compare direct checked block
/// invocation with the production session on the same authored guest state.
//===----------------------------------------------------------------------===//

#include "X87TranslationTestSupport.h"
#include "gtest/gtest.h"

#include "neverd/translate/NativeTranslationSession.h"
#include "neverd/translate/TranslationJITLinker.h"
#include "neverd/translate/TranslationObjectRequest.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

using namespace neverd;
using namespace neverd::translate;
using namespace neverd::translate::test;

namespace {

TEST(X87TranslationNative, FrozenObjectsMatchProductionSessionsAcrossControls) {
  if (!nativeLinuxAArch64())
    GTEST_SKIP() << "native Linux AArch64 JITLink execution required";
  std::unique_ptr<llvm::raw_fd_ostream> Records;
  std::filesystem::path Directory;
  if (const char *Path = std::getenv("NEVERD_X87_TRANSLATION_ARTIFACT_DIR")) {
    Directory = Path;
    std::filesystem::create_directories(Directory);
    std::error_code Error;
    Records = std::make_unique<llvm::raw_fd_ostream>(
        (Directory / "observations.jsonl").string(), Error);
    ASSERT_FALSE(Error) << Error.message();
  }
  const auto Registry =
      llvm::cantFail(RuntimeSymbolRegistryV1::create(Profile));
  for (auto Level : {LLVMOptimizationLevel::O0, LLVMOptimizationLevel::O2})
    for (unsigned Top = 0; Top != 8; ++Top)
      for (unsigned PC : {0u, 2u, 3u})
        for (unsigned RC = 0; RC != 4; ++RC) {
          const auto Case = "exact-" + std::to_string(Top) + "-" +
                            std::to_string(PC) + "-" + std::to_string(RC);
          const auto Artifact = Case + "-O" + std::to_string(unsigned(Level));
          SCOPED_TRACE(Artifact);
          auto Options = options(true);
          Options.LLVMLevel = Level;
          auto Initial = fixture(program(), Top, 0x7f | PC << 8 | RC << 10);
          auto Logical = Initial;
          auto State =
              llvm::cantFail(createRuntimeGuestStateX86_64X87V1(Logical));
          GuestMemoryRuntimeConfig Config;
          Config.CodeInvalidation = Options.CodeInvalidation;
          auto Memory =
              llvm::cantFail(GuestMemoryRuntime::create(Logical, Config));
          llvm::json::Array Blocks;
          for (unsigned Index = 0; Index != 6; ++Index) {
            auto Object = compileTranslationObjectRequestV1(
                {Logical, State.Integer.RIP, Options});
            ASSERT_TRUE(static_cast<bool>(Object))
                << llvm::toString(Object.takeError());
            for (const auto &Binding : Object->descriptor().GenerationBindings)
              ASSERT_EQ(Memory->generationForAddress(Binding.Address),
                        Binding.Generation);
            const RuntimeCodeCredentialV1 Credential{1, Index + 1u,
                                                     State.Integer.RIP, 1, 1};
            auto Linked =
                linkTranslationObjectV1(*Object, Registry, Credential);
            ASSERT_TRUE(static_cast<bool>(Linked))
                << llvm::toString(Linked.takeError());
            const auto Filename =
                Artifact + "-block-" + std::to_string(Index) + ".o";
            if (Records) {
              std::error_code Error;
              llvm::raw_fd_ostream File((Directory / Filename).string(), Error);
              ASSERT_FALSE(Error) << Error.message();
              const auto Bytes = Object->artifact().bytes();
              File.write(reinterpret_cast<const char *>(Bytes.data()),
                         Bytes.size());
              File.close();
              ASSERT_FALSE(File.has_error());
            }
            auto Frame = llvm::cantFail(
                createRuntimeCallFrameV1(*Memory, Credential, Credential));
            auto Status = Linked->invokePortableX87(State, Frame);
            ASSERT_TRUE(static_cast<bool>(Status))
                << llvm::toString(Status.takeError());
            EXPECT_EQ(*Status, static_cast<uint32_t>(
                                   Index == 5 ? BlockExitKindV1::Return
                                              : BlockExitKindV1::Continue));
            EXPECT_TRUE(Linked->auditReceipt().completed(
                TranslationJITLinkAuditStageV1::Finalized));
            EXPECT_TRUE(Linked->auditReceipt().InvocationCredentialBound);
            llvm::cantFail(applyRuntimeGuestStateX86_64X87V1(State, Logical));
            Logical.Memory = Memory->snapshotMemoryRegions();
            Blocks.push_back(llvm::json::Object{
                {"object", Filename},
                {"guest_bytes", llvm::toHex(Object->descriptor().Bytes)},
                {"entry_pc",
                 llvm::utohexstr(Object->descriptor().Header.EntryPC)},
                {"registry",
                 Object->artifact().runtimeRegistryIdentity().str()},
                {"artifact_identity",
                 Object->artifact().artifactCacheKey().str()},
                {"status", *Status},
                {"selector", State.LastStep.Selector},
                {"portable_status",
                 static_cast<uint32_t>(State.LastStep.Status)},
                {"attempted", bool(State.LastStep.Attempted)},
                {"completed", bool(State.LastStep.InstructionCompleted)},
                {"after",
                 llvm::toHex(llvm::cantFail(serializeGuestState(Logical)))},
                {"audit_stages", Linked->auditReceipt().CompletedStages}});
            llvm::cantFail(Linked->unload());
          }
          auto Session = llvm::cantFail(
              NativeTranslationSessionV1::create(Options, Initial));
          auto Result = Session->run();
          ASSERT_TRUE(static_cast<bool>(Result))
              << llvm::toString(Result.takeError());
          EXPECT_EQ(Result->Exit.Reason, TranslationStopReason::Returned);
          EXPECT_EQ(Result->GuestInstructions, 7u);
          EXPECT_EQ(Result->BlocksTranslated, 6u);
          const auto After = llvm::cantFail(serializeGuestState(Logical));
          const auto SessionAfter =
              llvm::cantFail(serializeGuestState(Session->state()));
          EXPECT_EQ(After, SessionAfter);
          EXPECT_EQ(State.Integer.RIP, 0x1234u);
          EXPECT_EQ(State.Integer.GPR[0], (Top << 11) | (PC != 3 ? 0x20u : 0));
          uint64_t Mantissa = 0xc000000000000000;
          if (PC == 3)
            Mantissa |= 1;
          else if (RC == 2)
            Mantissa |= uint64_t{1} << (PC == 2 ? 11 : 40);
          EXPECT_EQ(llvm::support::endian::read64le(
                        Logical.Memory[2].Bytes.data() + 1),
                    Mantissa);
          if (Records)
            *Records << llvm::json::Value(llvm::json::Object{
                            {"case", Case},
                            {"level", unsigned(Level)},
                            {"before", llvm::toHex(llvm::cantFail(
                                           serializeGuestState(Initial)))},
                            {"after", llvm::toHex(After)},
                            {"session_after", llvm::toHex(SessionAfter)},
                            {"exit_reason",
                             static_cast<unsigned>(Result->Exit.Reason)},
                            {"guest_instructions", Result->GuestInstructions},
                            {"blocks_translated", Result->BlocksTranslated},
                            {"generated_bytes", Result->GeneratedCodeBytes},
                            {"blocks", std::move(Blocks)}})
                     << '\n';
        }
  if (Records) {
    Records->close();
    ASSERT_FALSE(Records->has_error());
  }
}

} // namespace
