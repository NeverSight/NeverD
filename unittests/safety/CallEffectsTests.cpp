//===- CallEffectsTests.cpp - External-call summary catalog tests --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "CallEffects.h"
#include "gtest/gtest.h"

#include "neverd/safety/SinkCatalog.h"

#include <fstream>
#include <utility>

using namespace neverd;
using namespace neverd::safety;

TEST(CallEffects, ExactBoundedLengthFamilyRequiresCompleteArity) {
  const CallEffects Exact =
      resolveCallEffects("__strnlen", BinaryFormat::Unknown, Arch::Unknown, 2);
  ASSERT_TRUE(Exact);
  EXPECT_EQ(Exact.family(), CallEffectFamily::BoundedStringLength);
  EXPECT_TRUE(Exact.has(CallEffectCapability::StringExtent));
  EXPECT_TRUE(Exact.has(CallEffectCapability::Return));

  const CallEffects Truncated =
      resolveCallEffects("strnlen", BinaryFormat::Unknown, Arch::Unknown, 1);
  EXPECT_FALSE(Truncated);
  EXPECT_EQ(Truncated.capabilities(), CallEffectCapability::NoEffect);
}

TEST(CallEffects, SimilarLengthNameHasNoEffect) {
  const CallEffects Effects =
      resolveCallEffects("project_strnlen", BinaryFormat::ELF, Arch::X64, 2);
  EXPECT_FALSE(Effects);
  EXPECT_EQ(Effects.family(), CallEffectFamily::None);
  EXPECT_EQ(Effects.capabilities(), CallEffectCapability::NoEffect);
}

TEST(CallEffects, WindowsSummaryRequiresExactFormatAndABI) {
  const CallEffects Exact =
      resolveCallEffects("ReadFile", BinaryFormat::COFF, Arch::X64, 5);
  ASSERT_TRUE(Exact);
  EXPECT_EQ(Exact.family(), CallEffectFamily::Input);
  EXPECT_TRUE(Exact.has(CallEffectCapability::Taint));
  EXPECT_TRUE(Exact.has(CallEffectCapability::ModRef));
  EXPECT_TRUE(Exact.has(CallEffectCapability::MayConsumeStandardInput));
  EXPECT_FALSE(Exact.has(CallEffectCapability::DescriptorZeroIsStandardInput));

  EXPECT_FALSE(resolveCallEffects("ReadFile", BinaryFormat::ELF, Arch::X64, 5));
  EXPECT_FALSE(
      resolveCallEffects("ReadFile", BinaryFormat::COFF, Arch::Unknown, 5));
  EXPECT_FALSE(
      resolveCallEffects("ReadFile", BinaryFormat::COFF, Arch::X64, 4));
}

TEST(CallEffects, StandardInputFactsComeFromTheDescriptor) {
  const CallEffects Read =
      resolveCallEffects("read", BinaryFormat::ELF, Arch::X64, 3);
  ASSERT_TRUE(Read);
  EXPECT_TRUE(Read.has(CallEffectCapability::MayConsumeStandardInput));
  EXPECT_TRUE(Read.has(CallEffectCapability::DescriptorZeroIsStandardInput));
  EXPECT_TRUE(Read.has(CallEffectCapability::ExactStandardInputReplay));

  const CallEffects Environment =
      resolveCallEffects("getenv", BinaryFormat::ELF, Arch::X64, 1);
  ASSERT_TRUE(Environment);
  EXPECT_FALSE(Environment.has(CallEffectCapability::MayConsumeStandardInput));
  EXPECT_TRUE(Environment.has(CallEffectCapability::LiteralEnvironmentReplay));
}

TEST(CallEffects, UnknownDiscoveryNameDefaultsToNoEffect) {
  EXPECT_EQ(lookupCallEffectDescriptor("custom_input"), nullptr);
  const CallEffects Effects =
      resolveCallEffects("custom_input", BinaryFormat::MachO, Arch::AArch64, 3);
  EXPECT_FALSE(Effects);
  EXPECT_EQ(Effects.capabilities(), CallEffectCapability::NoEffect);
}

TEST(CallEffects, ConfiguredCopyProvidesExactDynamicEffect) {
  const std::string Path =
      std::string(::testing::TempDir()) + "/neverd_dynamic_copy.json";
  {
    std::ofstream OS(Path);
    OS << R"({"sinks":[{"name":"my_copy","kind":"copy",)"
       << R"("dst":0,"src":1,"len":2}]})";
  }

  SinkCatalog C = SinkCatalog::defaults();
  ASSERT_FALSE(static_cast<bool>(C.mergeSinksFromFile(Path)));
  const SinkEntry *Entry = C.matchSink("my_copy");
  ASSERT_NE(Entry, nullptr);

  const CallEffects Exact =
      resolveCallEffects(C, "my_copy", BinaryFormat::ELF, Arch::X64, 3);
  ASSERT_TRUE(Exact);
  EXPECT_EQ(Exact.family(), CallEffectFamily::Copy);
  EXPECT_TRUE(Exact.has(CallEffectCapability::ModRef));
  EXPECT_FALSE(Exact.has(CallEffectCapability::Capture));
  EXPECT_TRUE(Exact.supports(*Entry));

  EXPECT_FALSE(
      resolveCallEffects(C, "my_copy", BinaryFormat::ELF, Arch::X64, 2));
  EXPECT_FALSE(
      resolveCallEffects(C, "my_copy", BinaryFormat::ELF, Arch::X64, 4));
}

TEST(CallEffects, ConfiguredFormatHonorsExplicitApplicability) {
  const std::string Path =
      std::string(::testing::TempDir()) + "/neverd_dynamic_format.json";
  {
    std::ofstream OS(Path);
    OS << R"({"sinks":[{"name":"my_format","kind":"format",)"
       << R"("dst":0,"fmt":2,"effect":{"min_arity":4,)"
       << R"("max_arity":"variadic","formats":["elf"],)"
       << R"("abis":["sysv"]}}]})";
  }

  SinkCatalog C = SinkCatalog::defaults();
  ASSERT_FALSE(static_cast<bool>(C.mergeSinksFromFile(Path)));
  const SinkEntry *Entry = C.matchSink("my_format");
  ASSERT_NE(Entry, nullptr);

  const CallEffects Exact =
      resolveCallEffects(C, "my_format", BinaryFormat::ELF, Arch::X64, 7);
  ASSERT_TRUE(Exact);
  EXPECT_EQ(Exact.family(), CallEffectFamily::Format);
  EXPECT_TRUE(Exact.supports(*Entry));

  EXPECT_FALSE(
      resolveCallEffects(C, "my_format", BinaryFormat::ELF, Arch::X64, 2));
  EXPECT_FALSE(
      resolveCallEffects(C, "my_format", BinaryFormat::ELF, Arch::X64, 3));
  EXPECT_FALSE(
      resolveCallEffects(C, "my_format", BinaryFormat::COFF, Arch::X64, 7));
  EXPECT_FALSE(
      resolveCallEffects(C, "my_format", BinaryFormat::ELF, Arch::AArch64, 7));
}

TEST(CallEffects, ConfiguredUnboundedCopyProvidesOnlyWriteEffects) {
  const std::string Path =
      std::string(::testing::TempDir()) + "/neverd_dynamic_unbounded.json";
  {
    std::ofstream OS(Path);
    OS << R"({"sinks":[{"name":"my_unbounded","kind":"copy",)"
       << R"("dst":0,"unbounded":true}],)"
       << R"("sources":[{"name":"my_unbounded","out":0}]})";
  }

  SinkCatalog C = SinkCatalog::defaults();
  ASSERT_FALSE(static_cast<bool>(C.mergeSinksFromFile(Path)));
  ASSERT_FALSE(static_cast<bool>(C.mergeSourcesFromFile(Path)));
  const SinkEntry *Entry = C.matchSink("my_unbounded");
  ASSERT_NE(Entry, nullptr);

  const CallEffects Effects =
      resolveCallEffects(C, "my_unbounded", BinaryFormat::ELF, Arch::X64, 1);
  ASSERT_TRUE(Effects);
  EXPECT_EQ(Effects.family(), CallEffectFamily::Copy);
  EXPECT_TRUE(Effects.has(CallEffectCapability::ModRef));
  EXPECT_FALSE(Effects.has(CallEffectCapability::Taint));
  EXPECT_FALSE(Effects.has(CallEffectCapability::Capture));
  EXPECT_FALSE(Effects.has(CallEffectCapability::Return));
  EXPECT_TRUE(Effects.supports(*Entry));
  EXPECT_FALSE(
      resolveCallEffects(C, "my_unbounded", BinaryFormat::ELF, Arch::X64, 0));
  EXPECT_FALSE(
      resolveCallEffects(C, "my_unbounded", BinaryFormat::ELF, Arch::X64, 2));
}

TEST(CallEffects, ConfiguredSourceOverrideShadowsBuiltInEffects) {
  const std::string Path =
      std::string(::testing::TempDir()) + "/neverd_dynamic_source.json";
  {
    std::ofstream OS(Path);
    OS << R"({"sources":[{"name":"read","out":0}]})";
  }

  SinkCatalog C = SinkCatalog::defaults();
  ASSERT_FALSE(static_cast<bool>(C.mergeSourcesFromFile(Path)));
  ASSERT_NE(C.matchSource("read"), nullptr);
  EXPECT_EQ(C.matchSource("read")->OutArg, 0);

  EXPECT_TRUE(resolveCallEffects("read", BinaryFormat::ELF, Arch::X64, 3));
  const CallEffects Configured =
      resolveCallEffects(C, "read", BinaryFormat::ELF, Arch::X64, 3);
  EXPECT_FALSE(Configured);
  EXPECT_EQ(Configured.capabilities(), CallEffectCapability::NoEffect);
}

TEST(CallEffects, ConfiguredOverrideShadowsBuiltInAliasesWithoutFallback) {
  const std::string Path =
      std::string(::testing::TempDir()) + "/neverd_dynamic_override.json";
  {
    std::ofstream OS(Path);
    OS << R"({"sinks":[{"name":"memcpy","aliases":["project_copy"],)"
       << R"("kind":"copy","dst":2,"src":1,"len":0,)"
       << R"("effect":{"min_arity":4,"max_arity":4}}]})";
  }

  SinkCatalog C = SinkCatalog::defaults();
  ASSERT_FALSE(static_cast<bool>(C.mergeSinksFromFile(Path)));

  // The legacy resolver remains available for callers without a catalog.
  EXPECT_TRUE(
      resolveCallEffects("memcpy", BinaryFormat::ELF, Arch::AArch64, 3));
  // A configured applicability miss is final: neither the canonical name nor
  // preserved aliases may fall back to the closed-world descriptor.
  EXPECT_FALSE(
      resolveCallEffects(C, "memcpy", BinaryFormat::ELF, Arch::AArch64, 3));
  EXPECT_FALSE(resolveCallEffects(C, "__aeabi_memcpy", BinaryFormat::ELF,
                                  Arch::AArch64, 3));

  for (const char *Name : {"memcpy", "__aeabi_memcpy", "project_copy"}) {
    SCOPED_TRACE(Name);
    const CallEffects Effects =
        resolveCallEffects(C, Name, BinaryFormat::ELF, Arch::AArch64, 4);
    ASSERT_TRUE(Effects);
    EXPECT_EQ(Effects.family(), CallEffectFamily::Copy);
    const SinkEntry *Entry = C.matchSink(Name);
    ASSERT_NE(Entry, nullptr);
    EXPECT_TRUE(Effects.supports(*Entry));
  }
}

TEST(CallEffects, AliasRebindingDropsStaleConfiguredEffects) {
  const std::string Path =
      std::string(::testing::TempDir()) + "/neverd_dynamic_rebind.json";
  {
    std::ofstream OS(Path);
    OS << R"({"sinks":[{"name":"first_copy",)"
       << R"("aliases":["shared_copy"],"kind":"copy",)"
       << R"("dst":0,"src":1,"len":2},{"name":"second_copy",)"
       << R"("kind":"copy","dst":0,"src":1}]})";
  }

  SinkCatalog C = SinkCatalog::defaults();
  ASSERT_FALSE(static_cast<bool>(C.mergeSinksFromFile(Path)));
  EXPECT_TRUE(
      resolveCallEffects(C, "shared_copy", BinaryFormat::ELF, Arch::X64, 3));

  C.addSinkAlias("second_copy", "shared_copy");
  EXPECT_EQ(C.matchSink("shared_copy"), C.matchSink("second_copy"));
  EXPECT_FALSE(
      resolveCallEffects(C, "shared_copy", BinaryFormat::ELF, Arch::X64, 3));
  const CallEffects Rebound =
      resolveCallEffects(C, "shared_copy", BinaryFormat::ELF, Arch::X64, 2);
  ASSERT_TRUE(Rebound);
  const SinkEntry *ReboundEntry = C.matchSink("shared_copy");
  ASSERT_NE(ReboundEntry, nullptr);
  EXPECT_TRUE(Rebound.supports(*ReboundEntry));

  SinkEntry Untyped;
  Untyped.Name = "untyped_copy";
  Untyped.Kind = SinkKind::Copy;
  Untyped.DstArg = 0;
  Untyped.SrcArg = 1;
  C.addSink(std::move(Untyped));
  C.addSinkAlias("untyped_copy", "shared_copy");
  EXPECT_EQ(C.matchSink("shared_copy"), C.matchSink("untyped_copy"));
  EXPECT_FALSE(
      resolveCallEffects(C, "shared_copy", BinaryFormat::ELF, Arch::X64, 2));
}

TEST(CallEffects, ProgrammaticOverridesCannotBorrowBuiltInEffects) {
  SinkCatalog C = SinkCatalog::defaults();

  SinkEntry CopyOverride;
  CopyOverride.Name = "memcpy";
  CopyOverride.Kind = SinkKind::Copy;
  CopyOverride.DstArg = 2;
  CopyOverride.SrcArg = 1;
  CopyOverride.LenArg = 0;
  C.addSink(std::move(CopyOverride));
  EXPECT_FALSE(
      resolveCallEffects(C, "memcpy", BinaryFormat::ELF, Arch::X64, 3));
  EXPECT_FALSE(resolveCallEffects(C, "__aeabi_memcpy", BinaryFormat::ELF,
                                  Arch::AArch64, 3));

  C.addSource(SourceEntry{"read", 0});
  EXPECT_FALSE(resolveCallEffects(C, "read", BinaryFormat::ELF, Arch::X64, 3));
}

TEST(CallEffects, ProgrammaticAliasCannotBorrowBuiltInEffects) {
  SinkCatalog C = SinkCatalog::defaults();

  SinkEntry Wrapper;
  Wrapper.Name = "custom_copy";
  Wrapper.Aliases = {"memcpy"};
  Wrapper.Kind = SinkKind::Copy;
  Wrapper.DstArg = 1;
  Wrapper.SrcArg = 2;
  Wrapper.LenArg = 3;
  C.addSink(std::move(Wrapper));

  ASSERT_EQ(C.matchSink("memcpy"), C.matchSink("custom_copy"));
  EXPECT_FALSE(
      resolveCallEffects(C, "memcpy", BinaryFormat::ELF, Arch::X64, 3));
  EXPECT_FALSE(
      resolveCallEffects(C, "memcpy", BinaryFormat::ELF, Arch::X64, 4));
}

TEST(CallEffects, ConfiguredAliasCannotLeakIntoDisplacedCanonicalAliases) {
  const std::string Path =
      std::string(::testing::TempDir()) + "/neverd_dynamic_alias_steal.json";
  {
    std::ofstream OS(Path);
    OS << R"({"sinks":[{"name":"wrapper_copy","aliases":["memcpy"],)"
       << R"("kind":"copy","dst":0,"src":1,"len":2,)"
       << R"("effect":{"min_arity":4,"max_arity":4}}]})";
  }

  SinkCatalog C = SinkCatalog::defaults();
  ASSERT_FALSE(static_cast<bool>(C.mergeSinksFromFile(Path)));
  EXPECT_TRUE(resolveCallEffects(C, "wrapper_copy", BinaryFormat::ELF,
                                 Arch::AArch64, 4));
  EXPECT_TRUE(
      resolveCallEffects(C, "memcpy", BinaryFormat::ELF, Arch::AArch64, 4));

  EXPECT_FALSE(resolveCallEffects(C, "__aeabi_memcpy", BinaryFormat::ELF,
                                  Arch::AArch64, 4));
  const CallEffects BuiltIn = resolveCallEffects(
      C, "__aeabi_memcpy", BinaryFormat::ELF, Arch::AArch64, 3);
  ASSERT_TRUE(BuiltIn);
  EXPECT_TRUE(BuiltIn.supports(*C.matchSink("__aeabi_memcpy")));
}

TEST(CallEffects, InvalidSourceFileDoesNotPublishAnEffectShadow) {
  const std::string Path =
      std::string(::testing::TempDir()) + "/neverd_source_shadow_rollback.json";
  {
    std::ofstream OS(Path);
    OS << R"({"sources":[{"name":"read","out":0},)"
       << R"({"name":"bad","out":0,"return_tainted":"yes"}]})";
  }

  SinkCatalog C = SinkCatalog::defaults();
  llvm::Error Error = C.mergeSourcesFromFile(Path);
  ASSERT_TRUE(static_cast<bool>(Error));
  llvm::consumeError(std::move(Error));
  ASSERT_NE(C.matchSource("read"), nullptr);
  EXPECT_EQ(C.matchSource("read")->OutArg, 1);
  EXPECT_TRUE(resolveCallEffects(C, "read", BinaryFormat::ELF, Arch::X64, 3));
}
