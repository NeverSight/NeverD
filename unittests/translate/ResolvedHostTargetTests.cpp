//===- ResolvedHostTargetTests.cpp - Host target identity tests ----------===//

#include "gtest/gtest.h"

#include "neverd/translate/ResolvedHostTarget.h"

#include "llvm/Support/Error.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace neverd::translate;

namespace {

std::string takeError(llvm::Error Error) {
  return llvm::toString(std::move(Error));
}

TranslationOptions explicitTarget(GuestArchitecture Architecture,
                                  std::string Triple) {
  TranslationOptions Options;
  switch (Architecture) {
  case GuestArchitecture::X86_32:
  case GuestArchitecture::X86_64:
    Options.Guest = GuestArchitecture::ARM32;
    break;
  case GuestArchitecture::ARM32:
    Options.Guest = GuestArchitecture::X86_32;
    break;
  case GuestArchitecture::AArch64:
    Options.Guest = GuestArchitecture::X86_64;
    break;
  }
  Options.Mode = TranslationMode::AOT;
  Options.Target.Kind = HostTargetKind::Explicit;
  Options.Target.Architecture = Architecture;
  Options.Target.Triple = std::move(Triple);
  Options.BlockCache = BlockCachePolicy::Disabled;
  Options.CodeInvalidation = CodeInvalidationPolicy::RejectExecutableWrites;
  return Options;
}

std::optional<GuestArchitecture> architectureFromTriple(llvm::Triple Triple) {
  switch (Triple.getArch()) {
  case llvm::Triple::x86:
    return GuestArchitecture::X86_32;
  case llvm::Triple::x86_64:
    return GuestArchitecture::X86_64;
  case llvm::Triple::arm:
  case llvm::Triple::thumb:
    return GuestArchitecture::ARM32;
  case llvm::Triple::aarch64:
    return GuestArchitecture::AArch64;
  default:
    return std::nullopt;
  }
}

TEST(ResolvedHostTarget, SupportsEveryContractHostArchitecture) {
  struct TestCase {
    GuestArchitecture Architecture;
    const char *Triple;
  };
  constexpr TestCase Cases[] = {
      {GuestArchitecture::X86_32, "i686-pc-linux-gnu"},
      {GuestArchitecture::X86_64, "x86_64-pc-linux-gnu"},
      {GuestArchitecture::ARM32, "armv7-none-linux-gnueabihf"},
      {GuestArchitecture::AArch64, "aarch64-unknown-linux-gnu"},
  };

  for (const TestCase &Case : Cases) {
    TranslationOptions Options = explicitTarget(Case.Architecture, Case.Triple);
    llvm::Expected<ResolvedHostTarget> Target = resolveHostTarget(Options);
    if (!Target)
      FAIL() << Case.Triple << ": " << takeError(Target.takeError());
    EXPECT_EQ(Target->architecture(), Case.Architecture);
    EXPECT_EQ(llvm::Triple(Target->triple()).normalize(), Target->triple());
    EXPECT_FALSE(Target->cacheKey().empty());
  }
}

TEST(ResolvedHostTarget, CanonicalizesExplicitTripleAndPreservesRequest) {
  TranslationOptions Options =
      explicitTarget(GuestArchitecture::X86_64, "x86_64-linux-gnu");
  Options.Target.CPU = "znver4";
  Options.Target.Features = {"-avx", "+sse4.2", "+sse2"};

  llvm::Expected<ResolvedHostTarget> Target = resolveHostTarget(Options);
  if (!Target)
    FAIL() << takeError(Target.takeError());

  EXPECT_EQ(Target->requestedTarget().Triple, "x86_64-linux-gnu");
  EXPECT_EQ(Target->requestedTarget().Features, Options.Target.Features);
  EXPECT_EQ(Target->triple(), "x86_64-unknown-linux-gnu");
  EXPECT_EQ(Target->architecture(), GuestArchitecture::X86_64);
  EXPECT_EQ(Target->cpu(), "znver4");
  EXPECT_EQ(Target->features(),
            (llvm::ArrayRef<std::string>{"+sse2", "+sse4.2", "-avx"}));
  EXPECT_EQ(Target->cacheKey(), "neverd.host-target.v1\n"
                                "requested=explicit\n"
                                "architecture=x86_64\n"
                                "triple=x86_64-unknown-linux-gnu\n"
                                "cpu=znver4\n"
                                "features=3\n"
                                "feature=+sse2\n"
                                "feature=+sse4.2\n"
                                "feature=-avx\n");
}

TEST(ResolvedHostTarget, FeatureOrderDoesNotChangeCacheIdentity) {
  TranslationOptions First =
      explicitTarget(GuestArchitecture::X86_64, "x86_64-linux-gnu");
  First.Target.Features = {"+sse4.2", "-avx", "+sse2"};
  TranslationOptions Second = First;
  Second.Target.Features = {"+sse2", "+sse4.2", "-avx"};

  llvm::Expected<ResolvedHostTarget> FirstTarget = resolveHostTarget(First);
  llvm::Expected<ResolvedHostTarget> SecondTarget = resolveHostTarget(Second);
  if (!FirstTarget)
    FAIL() << takeError(FirstTarget.takeError());
  if (!SecondTarget)
    FAIL() << takeError(SecondTarget.takeError());

  EXPECT_NE(FirstTarget->requestedTarget().Features,
            SecondTarget->requestedTarget().Features);
  EXPECT_EQ(FirstTarget->features(), SecondTarget->features());
  EXPECT_EQ(FirstTarget->cacheKey(), SecondTarget->cacheKey());
}

TEST(ResolvedHostTarget, RejectsInvalidFeatureSets) {
  for (std::vector<std::string> Features :
       {std::vector<std::string>{""}, std::vector<std::string>{"+"},
        std::vector<std::string>{"sse2"},
        std::vector<std::string>{"--sse2"},
        std::vector<std::string>{"+sse2", "+sse2"},
        std::vector<std::string>{"+sse2", "-sse2"}}) {
    TranslationOptions Options =
        explicitTarget(GuestArchitecture::X86_64, "x86_64-linux-gnu");
    Options.Target.Features = std::move(Features);
    llvm::Expected<ResolvedHostTarget> Target = resolveHostTarget(Options);
    ASSERT_FALSE(Target);
    EXPECT_FALSE(takeError(Target.takeError()).empty());
  }
}

TEST(ResolvedHostTarget, RejectsMismatchedAndUnsupportedTriples) {
  TranslationOptions Mismatch =
      explicitTarget(GuestArchitecture::AArch64, "x86_64-linux-gnu");
  llvm::Expected<ResolvedHostTarget> MismatchedTarget =
      resolveHostTarget(Mismatch);
  ASSERT_FALSE(MismatchedTarget);
  EXPECT_NE(takeError(MismatchedTarget.takeError()).find("does not match"),
            std::string::npos);

  TranslationOptions Unsupported =
      explicitTarget(GuestArchitecture::AArch64, "riscv64-linux-gnu");
  llvm::Expected<ResolvedHostTarget> UnsupportedTarget =
      resolveHostTarget(Unsupported);
  ASSERT_FALSE(UnsupportedTarget);
  EXPECT_NE(takeError(UnsupportedTarget.takeError()).find("unsupported"),
            std::string::npos);

  TranslationOptions Incomplete =
      explicitTarget(GuestArchitecture::X86_64, "x86_64");
  llvm::Expected<ResolvedHostTarget> IncompleteTarget =
      resolveHostTarget(Incomplete);
  ASSERT_FALSE(IncompleteTarget);
  EXPECT_NE(takeError(IncompleteTarget.takeError()).find("complete canonical"),
            std::string::npos);
}

TEST(ResolvedHostTarget, EnforcesJITAndAOTTargetKinds) {
  TranslationOptions ExplicitJIT;
  ExplicitJIT.Target.Kind = HostTargetKind::Explicit;
  ExplicitJIT.Target.Architecture = GuestArchitecture::AArch64;
  ExplicitJIT.Target.Triple = "aarch64-unknown-linux-gnu";
  llvm::Expected<ResolvedHostTarget> JITTarget = resolveHostTarget(ExplicitJIT);
  ASSERT_FALSE(JITTarget);
  EXPECT_NE(takeError(JITTarget.takeError()).find("JIT translation"),
            std::string::npos);

  TranslationOptions NativeAOT;
  NativeAOT.Mode = TranslationMode::AOT;
  llvm::Expected<ResolvedHostTarget> AOTTarget = resolveHostTarget(NativeAOT);
  ASSERT_FALSE(AOTTarget);
  EXPECT_NE(takeError(AOTTarget.takeError()).find("AOT translation"),
            std::string::npos);
}

TEST(ResolvedHostTarget, ResolvesTheNativeProcessIdentity) {
  TranslationOptions Options;
  llvm::Expected<ResolvedHostTarget> Target = resolveHostTarget(Options);
  if (!Target)
    FAIL() << takeError(Target.takeError());

  const llvm::Triple ProcessTriple(
      llvm::Triple::normalize(llvm::sys::getProcessTriple()));
  const std::optional<GuestArchitecture> ProcessArchitecture =
      architectureFromTriple(ProcessTriple);
  ASSERT_TRUE(ProcessArchitecture);

  EXPECT_EQ(Target->requestedTarget().Kind, HostTargetKind::Native);
  EXPECT_TRUE(Target->requestedTarget().Triple.empty());
  EXPECT_EQ(Target->architecture(), *ProcessArchitecture);
  EXPECT_EQ(llvm::Triple(Target->triple()).getArch(), ProcessTriple.getArch());
  EXPECT_FALSE(Target->triple().empty());
  EXPECT_FALSE(Target->cpu().empty());
  EXPECT_TRUE(
      std::is_sorted(Target->features().begin(), Target->features().end()));
  EXPECT_TRUE(Target->cacheKey().starts_with("neverd.host-target.v1\n"));
}

} // namespace
