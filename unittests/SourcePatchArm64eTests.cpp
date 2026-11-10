//===- SourcePatchArm64eTests.cpp - Authenticated source SDK tests -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPI.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <iterator>
#include <optional>

namespace {
#define MACHO_AUTH_NATIVE_TEXT(Name, Value) constexpr char Name[] = Value;
#define MACHO_AUTH_NATIVE_VALUE(Type, Name, Value) constexpr Type Name = Value;
#define MACHO_AUTH_NATIVE_ARGS(Name, ...)                                      \
  constexpr llvm::StringLiteral Name[] = {__VA_ARGS__};
#include "lift/format/MachOAuthenticatedNativeFixtures.def"

enum class ArtifactProfile { GeneratedIR, NativeObject, IncompatibleCFI };
class SourcePatchArm64e : public testing::TestWithParam<ArtifactProfile> {};

TEST_P(SourcePatchArm64e, SourceCompilerEnforcesAuthenticatedProviderABI) {
#ifndef __APPLE__
  GTEST_SKIP() << kMissingHost;
#else
  const auto *IRCompiler = std::getenv(kIRCompilerEnvironment);
  const bool Generated = GetParam() == ArtifactProfile::GeneratedIR;
  const bool Incompatible = GetParam() == ArtifactProfile::IncompatibleCFI;
  if (Generated && !IRCompiler)
    GTEST_SKIP() << kMissingIRCompiler;
  llvm::SmallString<kPathCapacity> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory(kObjectArtifactPrefix, Directory));
  const auto Cleanup = llvm::scope_exit(
      [&] { EXPECT_FALSE(llvm::sys::fs::remove_directories(Directory)); });
  const auto Path = [&](llvm::StringRef Name) {
    auto Value = Directory;
    llvm::sys::path::append(Value, Name);
    return Value.str().str();
  };
  const auto Source = Path(kSourceFile), Original = Path(kOriginalFile),
             Output = Path(kInstalledFile);
  std::error_code Error;
  llvm::raw_fd_ostream OS(Source, Error);
  ASSERT_FALSE(Error);
  OS << kSource;
  OS.close();
  ASSERT_FALSE(OS.has_error());
  llvm::SmallVector<llvm::StringRef> Args{kCompiler};
  Args.append(std::begin(kCompileArgs), std::end(kCompileArgs));
  Args.append({kO0, Source});
  Args.append(std::begin(kOutputArgs), std::end(kOutputArgs));
  Args.push_back(Original);
  ASSERT_EQ(llvm::sys::ExecuteAndWait(kCompiler, Args, std::nullopt, {},
                                      kToolTimeout),
            0);
  auto Before = llvm::MemoryBuffer::getFile(Original);
  ASSERT_TRUE(bool(Before));
  auto Object = llvm::object::ObjectFile::createObjectFile(Original);
  ASSERT_TRUE(bool(Object)) << llvm::toString(Object.takeError());
  for (const auto &Section : Object->getBinary()->sections()) {
    auto Name = Section.getName();
    ASSERT_TRUE(bool(Name)) << llvm::toString(Name.takeError());
    EXPECT_NE(*Name, kEHFrameSection);
  }
  const char *Names[] = {kObjectDispatch, kObjectCatch, kObjectCleanup};
  neverd_source_patch_mapping_v1 Mappings[std::size(Names)]{};
  for (size_t I = 0; I < std::size(Names); ++I) {
    Mappings[I].symbol = Names[I];
    for (const auto &Symbol : Object->getBinary()->symbols()) {
      auto Name = Symbol.getName();
      ASSERT_TRUE(bool(Name)) << llvm::toString(Name.takeError());
      if (*Name != kPublishedEntries[I])
        continue;
      auto Address = Symbol.getAddress();
      ASSERT_TRUE(bool(Address)) << llvm::toString(Address.takeError());
      Mappings[I].original_va = *Address;
    }
    ASSERT_NE(Mappings[I].original_va, 0u);
  }
  auto Session = neverd_session_create();
  ASSERT_NE(Session, nullptr);
  const auto Destroy =
      llvm::scope_exit([&] { neverd_session_destroy(Session); });
  ASSERT_EQ(neverd_session_load(Session, Original.c_str()), 1)
      << neverd_last_error(Session);
  neverd_source_patch_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  Options.language = NEVERD_SOURCE_PATCH_CXX;
  Options.compiler_path = Generated ? IRCompiler : kCompiler;
  Options.artifact = Generated ? NEVERD_SOURCE_PATCH_ARTIFACT_IR
                               : NEVERD_SOURCE_PATCH_ARTIFACT_OBJECT;
  const char *CompilerArgs[] = {Incompatible ? kExplicitReturnCFIOption
                                             : kDwarfLanguageOption};
  if (Generated || Incompatible) {
    Options.compiler_args = CompilerArgs;
    Options.compiler_arg_count = std::size(CompilerArgs);
  }
  Options.mappings = Mappings;
  Options.mapping_count = std::size(Mappings);
  const neverd_source_patch_dependency_v1 Dependencies[] = {
      {kProviderLibrary, kProviderFunction, NEVERD_SOURCE_DEPENDENCY_FUNCTION},
      {kProviderLibrary, kProviderData, NEVERD_SOURCE_DEPENDENCY_DATA}};
  Options.dependencies = Dependencies;
  Options.dependency_count = std::size(Dependencies);
  const auto Result =
      neverd_patch_source(Session, kObjectSource, sizeof(kObjectSource) - 1,
                          &Options, Output.c_str());
  auto After = llvm::MemoryBuffer::getFile(Original);
  ASSERT_TRUE(bool(After));
  EXPECT_EQ((*After)->getBuffer(), (*Before)->getBuffer());
  if (Incompatible || (Generated && !Result &&
                       llvm::StringRef(neverd_last_error(Session))
                           .contains(kClassicReturnCFIDiagnostic))) {
    EXPECT_EQ(Result, 0);
    EXPECT_FALSE(llvm::sys::fs::exists(Output));
    EXPECT_TRUE(llvm::StringRef(neverd_last_error(Session))
                    .contains(kClassicReturnCFIDiagnostic));
    if (Generated)
      GTEST_SKIP() << kMissingCompatibleIRProducer;
    return;
  }
  ASSERT_EQ(Result, 1) << neverd_last_error(Session);
  EXPECT_TRUE(llvm::sys::fs::exists(Output));
  if (Generated) {
    auto Installed = llvm::object::ObjectFile::createObjectFile(Output);
    ASSERT_TRUE(bool(Installed)) << llvm::toString(Installed.takeError());
    bool Found = false;
    for (const auto &Section : Installed->getBinary()->sections()) {
      auto Name = Section.getName();
      ASSERT_TRUE(bool(Name)) << llvm::toString(Name.takeError());
      Found |= *Name == kEHFrameSection && Section.getSize() != 0;
    }
    EXPECT_TRUE(Found);
  }
  EXPECT_EQ(neverd_patch_source(Session, kObjectSource,
                                sizeof(kObjectSource) - 1, &Options,
                                Output.c_str()),
            0);
#endif
}

INSTANTIATE_TEST_SUITE_P(Native, SourcePatchArm64e,
                         ::testing::Values(ArtifactProfile::GeneratedIR,
                                           ArtifactProfile::NativeObject,
                                           ArtifactProfile::IncompatibleCFI));
} // namespace
