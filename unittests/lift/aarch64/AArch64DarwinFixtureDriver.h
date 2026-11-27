//===- AArch64DarwinFixtureDriver.h - Compile a Darwin ABI test caller ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Build the caller for the selected Darwin execution profile. Its dylib
/// always carries the arm64e ABI, including when an arm64 process loads it.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LIFT_AARCH64_AARCH64DARWINFIXTUREDRIVER_H
#define NEVERD_UNITTESTS_LIFT_AARCH64_AARCH64DARWINFIXTUREDRIVER_H

#include "gtest/gtest.h"

#include "neverd/loader/Loader.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/Program.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace neverd::test {

inline void buildDarwinFixtureDriver(const std::string &Executable,
                                     llvm::StringRef Body) {
#define DARWIN_DRIVER_TEXT(Name, Value) static constexpr char Name[] = Value;
#include "AArch64DarwinFixtureDriver.def"
  const auto Source =
      (std::filesystem::path(Executable).parent_path() / SourceName).string();
  std::ofstream(Source) << Body.str();
  const char *Selected = std::getenv(Profile);
  const bool Native = Selected && llvm::StringRef(Selected) == SystemProfile;
  const auto *Target = Native ? NativeTarget : HostTarget;
  ASSERT_EQ(llvm::sys::ExecuteAndWait(Compiler,
                                      {Compiler, TargetFlag, Target, Standard,
                                       Optimize, Source, Output, Executable}),
            0);
  auto Image = Loader::create(BinaryFormat::MachO)->load(Executable);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  ASSERT_EQ(Image->Arch, Arch::AArch64);
  ASSERT_EQ(Image->isArm64e(), Native);
  ::testing::Test::RecordProperty(Architecture, Native ? NativeName : HostName);
}

} // namespace neverd::test

#endif // NEVERD_UNITTESTS_LIFT_AARCH64_AARCH64DARWINFIXTUREDRIVER_H
