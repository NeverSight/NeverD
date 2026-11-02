//===- X87TranslationResetTests.cpp - Checked x87 reset sessions
//-----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compare FNOP/EMMS/FEMMS JITLink objects and production sessions with
/// explicit state transitions, including the integer prefix of a refused
/// instruction.
//===----------------------------------------------------------------------===//

#include "X87TranslationTestSupport.h"
#include "gtest/gtest.h"

#include "neverd/translate/NativeTranslationSession.h"
#include "neverd/translate/TranslationJITLinker.h"
#include "neverd/translate/TranslationLinkGraphVerifier.h"
#include "neverd/translate/TranslationObjectRequest.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

using namespace neverd;
using namespace neverd::translate;
using namespace neverd::translate::test;

namespace {

constexpr std::array<std::array<uint8_t, 2>, 3> Opcodes = {
    {{0xd9, 0xd0}, {0x0f, 0x77}, {0x0f, 0x0e}}};

std::vector<uint8_t> resetProgram(unsigned Kind) {
  return {0x48,
          0xc7,
          0xc0,
          0x22,
          0x11,
          0,
          0, // MOV RAX, 0x1122.
          Opcodes[Kind][0],
          Opcodes[Kind][1],
          0xc3};
}

GuestState resetFixture(unsigned Kind, unsigned Case) {
  const unsigned Top = Case < 72 ? Case / 9 : 5;
  auto Initial = fixture(resetProgram(Kind), Top);
  auto State = llvm::cantFail(createRuntimeGuestStateX86_64X87V1(Initial));
  auto &X87 = State.X87;
  for (unsigned Index = 32; Index != 160; ++Index)
    X87.Image[Index] = Index ^ 0xa5;
  llvm::support::endian::write16le(X87.Image.data() + 2, 0x4725 | Top << 11);
  llvm::support::endian::write16le(X87.Image.data() + 6, 0x571);
  llvm::support::endian::write64le(X87.Image.data() + 8, 0x45);
  llvm::support::endian::write64le(X87.Image.data() + 16, 0x67);
  constexpr uint8_t Tags[] = {0, 0x55, 0xff};
  constexpr uint8_t Known[] = {0, 0x96, 0xff};
  X87.Image[4] = Case < 72 ? Tags[Case / 3 % 3] : 0xff;
  X87.KnownPayload = Case < 72 ? Known[Case % 3] : 0xff;
  if (Case == 72)
    X87.EnvironmentKnown = 0;
  if (Case == 73) {
    X87.Image[0] &= ~1u;
    X87.Image[2] |= 0x81;
  }
  if (Case == 74)
    X87.Image[1] = 1;
  State.Integer.GPR[0] = 0xdecafbad;
  State.Integer.CF = State.Integer.ZF = 1;
  llvm::cantFail(applyRuntimeGuestStateX86_64X87V1(State, Initial));
  llvm::cantFail(setRegisterValue(Initial, X87ID + 1, llvm::APInt(64, 0xface),
                                  "neverd.test.untouched"));
  return Initial;
}

// Authored architectural expectation, independent of the production TOP helper
// and portable step. FXSAVE slots are logical; the tag and known masks are
// physical.
GuestState expectedReset(const GuestState &Initial, unsigned Kind, bool Refused,
                         bool Returned) {
  auto Expected = Initial;
  auto State = llvm::cantFail(createRuntimeGuestStateX86_64X87V1(Expected));
  State.Integer.GPR[0] = 0x1122;
  State.Integer.RIP = Entry + (Refused ? 7 : 9);
  if (!Refused) {
    if (Kind == 0) {
      llvm::support::endian::write64le(State.X87.Image.data() + 8, Entry + 7);
    } else {
      const auto Image = State.X87.Image;
      const unsigned Top = (Image[3] >> 3) & 7;
      for (unsigned Physical = 0; Physical != 8; ++Physical)
        std::copy_n(Image.begin() + 32 + ((Physical + 8 - Top) % 8) * 16, 16,
                    State.X87.Image.begin() + 32 + Physical * 16);
      State.X87.Image[3] &= 0xc7;
      State.X87.Image[4] = 0;
      if (Kind == 2)
        State.X87.KnownPayload = 0;
    }
    if (Returned) {
      State.Integer.RIP = 0x1234;
      State.Integer.GPR[4] += 8;
    }
  }
  llvm::cantFail(applyRuntimeGuestStateX86_64X87V1(State, Expected));
  return Expected;
}

TEST(X87Translation, ResetObjectsRetainTheInstructionAndOwnedProfile) {
  for (unsigned Kind = 0; Kind != Opcodes.size(); ++Kind) {
    SCOPED_TRACE(Kind);
    auto Initial = resetFixture(Kind, 0);
    auto Object =
        compileTranslationObjectRequestV1({Initial, Entry, options()});
    ASSERT_TRUE(static_cast<bool>(Object))
        << llvm::toString(Object.takeError());
    const auto &Descriptor = Object->descriptor();
    EXPECT_EQ(Descriptor.Header.Terminator,
              TranslationBlockTerminatorKindV1::X87Runtime);
    EXPECT_EQ(Descriptor.Header.GuestInstructionCount, 2u);
    EXPECT_EQ(Descriptor.Header.FallthroughPC, Entry + 9);
    const auto Bytes = resetProgram(Kind);
    EXPECT_EQ(llvm::ArrayRef<uint8_t>(Descriptor.Bytes),
              llvm::ArrayRef<uint8_t>(Bytes).drop_back());
    auto Audit = verifyTranslationLinkGraphV1(Object->artifact());
    ASSERT_TRUE(static_cast<bool>(Audit)) << llvm::toString(Audit.takeError());
    std::erase_if(Initial.Registers,
                  [](const auto &Register) { return Register.ID == X87ID; });
    auto Missing =
        compileTranslationObjectRequestV1({Initial, Entry, options()});
    ASSERT_FALSE(static_cast<bool>(Missing));
    llvm::consumeError(Missing.takeError());
  }
}

void checkResetSessions(unsigned Kind) {
  if (!nativeLinuxAArch64())
    GTEST_SKIP()
        << "native Linux AArch64 JITLink and session execution required";
  std::filesystem::path Directory;
  std::unique_ptr<llvm::raw_fd_ostream> Records;
  if (const char *Path = std::getenv("NEVERD_X87_TRANSLATION_ARTIFACT_DIR")) {
    Directory = Path;
    std::filesystem::create_directories(Directory);
    std::error_code Error;
    Records = std::make_unique<llvm::raw_fd_ostream>(
        (Directory / ("reset-observations-" + std::to_string(Kind) + ".jsonl"))
            .string(),
        Error);
    ASSERT_FALSE(Error) << Error.message();
  }
  const auto Registry =
      llvm::cantFail(RuntimeSymbolRegistryV1::create(Profile));
  for (auto Level : {LLVMOptimizationLevel::O0, LLVMOptimizationLevel::O2})
    for (unsigned Case = 0; Case != 75; ++Case) {
      const bool Refused = Case >= 72;
      const auto Name = "reset-" + std::to_string(Kind) + "-" +
                        std::to_string(Case) + "-O" +
                        std::to_string(unsigned(Level));
      SCOPED_TRACE(Name);
      auto Options = options(true);
      Options.LLVMLevel = Level;
      auto Initial = resetFixture(Kind, Case);
      auto Logical = Initial;
      auto State = llvm::cantFail(createRuntimeGuestStateX86_64X87V1(Logical));
      GuestMemoryRuntimeConfig Config;
      Config.CodeInvalidation = Options.CodeInvalidation;
      auto Memory = llvm::cantFail(GuestMemoryRuntime::create(Logical, Config));
      llvm::json::Array Blocks;
      for (unsigned Index = 0; Index != (Refused ? 1u : 2u); ++Index) {
        auto Object = compileTranslationObjectRequestV1(
            {Logical, State.Integer.RIP, Options});
        ASSERT_TRUE(static_cast<bool>(Object))
            << llvm::toString(Object.takeError());
        const RuntimeCodeCredentialV1 Credential{1, Index + 1u,
                                                 State.Integer.RIP, 1, 1};
        auto Linked = linkTranslationObjectV1(*Object, Registry, Credential);
        ASSERT_TRUE(static_cast<bool>(Linked))
            << llvm::toString(Linked.takeError());
        const auto Filename = Name + "-block-" + std::to_string(Index) + ".o";
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
        EXPECT_EQ(*Status,
                  static_cast<uint32_t>(Refused ? BlockExitKindV1::Unsupported
                                        : Index ? BlockExitKindV1::Return
                                                : BlockExitKindV1::Continue));
        if (!Index) {
          constexpr PortableX87StepStatusV1 Refusals[] = {
              PortableX87StepStatusV1::UnknownEnvironment,
              PortableX87StepStatusV1::PendingException,
              PortableX87StepStatusV1::InvalidControl};
          EXPECT_EQ(State.LastStep.Status,
                    Refused ? Refusals[Case - 72]
                            : PortableX87StepStatusV1::Completed);
          EXPECT_EQ(State.LastStep.PC, Entry + 7);
          EXPECT_EQ(State.LastStep.NextPC, Entry + 9);
          EXPECT_TRUE(State.LastStep.Attempted);
          EXPECT_EQ(bool(State.LastStep.InstructionCompleted), !Refused);
          EXPECT_FALSE(State.LastStep.HasScalarResult);
        }
        EXPECT_TRUE(Linked->auditReceipt().completed(
            TranslationJITLinkAuditStageV1::Finalized));
        EXPECT_TRUE(Linked->auditReceipt().InvocationCredentialBound);
        llvm::cantFail(applyRuntimeGuestStateX86_64X87V1(State, Logical));
        Logical.Memory = Memory->snapshotMemoryRegions();
        const auto Expected = expectedReset(Initial, Kind, Refused, Index == 1);
        const auto After = llvm::cantFail(serializeGuestState(Logical));
        EXPECT_EQ(After, llvm::cantFail(serializeGuestState(Expected)));
        Blocks.push_back(llvm::json::Object{
            {"object", Filename},
            {"guest_bytes", llvm::toHex(Object->descriptor().Bytes)},
            {"entry_pc", llvm::utohexstr(Object->descriptor().Header.EntryPC)},
            {"registry", Object->artifact().runtimeRegistryIdentity().str()},
            {"artifact_identity", Object->artifact().artifactCacheKey().str()},
            {"status", *Status},
            {"portable_status", static_cast<uint32_t>(State.LastStep.Status)},
            {"attempted", bool(State.LastStep.Attempted)},
            {"completed", bool(State.LastStep.InstructionCompleted)},
            {"after", llvm::toHex(After)},
            {"audit_stages", Linked->auditReceipt().CompletedStages}});
        llvm::cantFail(Linked->unload());
      }
      auto Session =
          llvm::cantFail(NativeTranslationSessionV1::create(Options, Initial));
      auto Result = Session->run();
      ASSERT_TRUE(static_cast<bool>(Result))
          << llvm::toString(Result.takeError());
      EXPECT_EQ(Result->Exit.Reason,
                Refused ? TranslationStopReason::UnsupportedInstruction
                        : TranslationStopReason::Returned);
      EXPECT_EQ(Result->GuestInstructions, Refused ? 2u : 3u);
      EXPECT_EQ(Result->BlocksTranslated, Refused ? 1u : 2u);
      if (Refused) {
        ASSERT_TRUE(Result->Exit.Trap);
        EXPECT_EQ(Result->Exit.Trap->Code, kRuntimeX87RefusalTrapCodeV1);
        EXPECT_EQ(Result->Exit.Trap->Subcode,
                  static_cast<uint32_t>(State.LastStep.Status));
        EXPECT_TRUE(Result->Exit.Trap->Restartable);
        EXPECT_EQ(Result->Exit.PC, Entry + 7);
        EXPECT_EQ(Result->Exit.NextPC, Entry + 7);
      }
      const auto After = llvm::cantFail(serializeGuestState(Logical));
      const auto SessionAfter =
          llvm::cantFail(serializeGuestState(Session->state()));
      EXPECT_EQ(After, SessionAfter);
      if (Records)
        *Records
            << llvm::json::Value(llvm::json::Object{
                   {"kind", Kind},
                   {"case", Case},
                   {"level", unsigned(Level)},
                   {"before",
                    llvm::toHex(llvm::cantFail(serializeGuestState(Initial)))},
                   {"after", llvm::toHex(After)},
                   {"session_after", llvm::toHex(SessionAfter)},
                   {"exit_reason", static_cast<unsigned>(Result->Exit.Reason)},
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

TEST(X87TranslationNative, FnopObjectsAndSessionsRetainStateAndRefusedPrefix) {
  checkResetSessions(0);
}

TEST(X87TranslationNative, EmmsObjectsAndSessionsRetainStateAndRefusedPrefix) {
  checkResetSessions(1);
}

TEST(X87TranslationNative, FemmsObjectsAndSessionsRetainStateAndRefusedPrefix) {
  checkResetSessions(2);
}

} // namespace
