//===- DriverModuleInputTests.cpp - Pinned driver/kernel observations
//------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Pinned driver/kernel observations.
///

#include "gtest/gtest.h"
#include "os/windows/driver/DriverExecutionRun.h"

#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"

#include <fstream>

using namespace neverd::emulation;
namespace {
namespace m = neverd::modules;
constexpr uint64_t MemoryLimit = 64 * 1024 * 1024;
std::filesystem::path fixture(const char *Name,
                              const char *Extension = ".sys") {
  return std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
         (std::string(Name) + Extension);
}
llvm::Expected<ExecutionRunEvent> advance(ExecutionRun &Run) {
  for (unsigned N = 0; N < 100000; ++N) {
    auto E = Run.run();
    if (!E)
      return E.takeError();
    if (E->State == ExecutionRunState::Paused &&
        E->Reason == ExecutionRunReason::Quantum) {
      if (auto Error = Run.resume())
        return std::move(Error);
      continue;
    }
    return E;
  }
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "resource test exceeded action bound");
}
class DriverModuleInput : public testing::Test {
protected:
  std::filesystem::path Root;
  std::shared_ptr<m::ModuleSet> Sources;
  void SetUp() override {
    llvm::SmallString<128> Temporary;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-driver-inputs",
                                                      Temporary));
    Root = std::u8string(Temporary.begin(), Temporary.end());
    m::ModuleOptions O;
    O.BaseDirectory = Root;
    Sources = llvm::cantFail(m::ModuleSet::create(std::move(O)));
  }
  void TearDown() override {
    Sources.reset();
    std::error_code EC;
    std::filesystem::remove_all(Root, EC);
  }
  std::shared_ptr<const m::ModuleImage>
  prepare(const char *ID, const char *Name, bool WithPDB = false) {
    m::ModuleInput I{ID, Root / (std::string(ID) + ".sys")};
    std::filesystem::copy_file(fixture(Name), I.Image);
    if (WithPDB) {
      auto P = Root / (std::string(ID) + ".pdb");
      std::filesystem::copy_file(fixture(Name, ".pdb"), P);
      I.Debug = m::DebugInput{m::DebugKind::PDB, P};
    }
    if (auto E = Sources->add(std::move(I))) {
      ADD_FAILURE() << llvm::toString(std::move(E));
      return nullptr;
    }
    auto S = Sources->prepare(ID);
    if (!S) {
      ADD_FAILURE() << llvm::toString(S.takeError());
      return nullptr;
    }
    return std::move(*S);
  }
  void same(const DriverImage &A, const DriverImage &B) {
    EXPECT_EQ(A.Base, B.Base);
    EXPECT_EQ(A.PreferredBase, B.PreferredBase);
    EXPECT_EQ(A.Entry, B.Entry);
    EXPECT_EQ(A.Size, B.Size);
    EXPECT_EQ(A.SecurityCookieAddress, B.SecurityCookieAddress);
    EXPECT_EQ(A.DLLCharacteristics, B.DLLCharacteristics);
    EXPECT_EQ(A.Guard.Enabled, B.Guard.Enabled);
    EXPECT_EQ(A.Guard.CheckPointerAddress, B.Guard.CheckPointerAddress);
    EXPECT_EQ(A.Guard.DispatchPointerAddress, B.Guard.DispatchPointerAddress);
    EXPECT_EQ(A.Guard.ValidTargets, B.Guard.ValidTargets);
    ASSERT_EQ(A.Regions.size(), B.Regions.size());
    for (size_t I = 0; I < A.Regions.size(); ++I) {
      EXPECT_EQ(A.Regions[I].Address, B.Regions[I].Address);
      EXPECT_EQ(A.Regions[I].Permissions, B.Regions[I].Permissions);
      EXPECT_EQ(A.Regions[I].Bytes, B.Regions[I].Bytes);
    }
    ASSERT_EQ(A.Imports.size(), B.Imports.size());
    for (size_t I = 0; I < A.Imports.size(); ++I) {
      EXPECT_EQ(A.Imports[I].Slot, B.Imports[I].Slot);
      EXPECT_EQ(A.Imports[I].Module, B.Imports[I].Module);
      EXPECT_EQ(A.Imports[I].Name, B.Imports[I].Name);
    }
  }
};
TEST_F(DriverModuleInput, FileAndPinnedImageShareStrictRebaseAndCookiePlans) {
  auto Source = prepare("driver", "loader");
  ASSERT_TRUE(Source);
  for (uint64_t Base :
       {0ULL, 0x140000000ULL, 0x280000000ULL, 0xfffff80010000000ULL}) {
    auto File = loadDriverImage(Source->sourcePath(), MemoryLimit, Base);
    auto Pinned = loadDriverImage(*Source, MemoryLimit, Base);
    ASSERT_TRUE(bool(File)) << llvm::toString(File.takeError());
    ASSERT_TRUE(bool(Pinned)) << llvm::toString(Pinned.takeError());
    same(*File, *Pinned);
  }
}
TEST_F(DriverModuleInput,
       RemovedSourceFilesAndCatalogueDoNotChangeActualCalls) {
  auto Source = prepare("driver", "driver_invocation", true);
  ASSERT_TRUE(Source);
  auto Function = Source->lookupSymbol("ProbeSum");
  ASSERT_TRUE(bool(Function));
  ASSERT_TRUE(*Function);
  const auto OriginalAddress = (**Function).Address;
  ASSERT_TRUE(std::filesystem::remove(Source->sourcePath()));
  ASSERT_TRUE(std::filesystem::remove(Source->debugSourcePath()));
  Sources.reset();
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64}) {
    DriverOptions O;
    O.Contract = Contract;
    O.LoadAddress = 0x190000000;
    ExecutionRunOptions C;
    C.Limits = {O.InstructionLimit, O.EventLimit, O.TimeoutMilliseconds * 1000};
    C.PauseStages = {"after-entry"};
    C.EnableResourcePauses = false;
    C.InstructionQuantum = 2;
    C.ActionsPerRun = 1;
    auto P = createDriverExecutionRun({}, O, C, {}, {Source, {}});
    ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
    EXPECT_EQ(P->State->Inputs.Image, Source);
    ASSERT_EQ(advance(*P->Run)->Reason, ExecutionRunReason::Stage);
    GuestInvocationRequest R;
    R.ABI = IntegerCallingConvention::Win64;
    R.Entry = OriginalAddress - P->State->Image->PreferredBase + O.LoadAddress;
    for (uint64_t I = 1; I <= 7; ++I)
      R.Arguments.push_back({I, {}});
    ASSERT_EQ(llvm::toString(P->Run->invoke(std::move(R))), "");
    auto E = advance(*P->Run);
    ASSERT_TRUE(bool(E));
    ASSERT_EQ(E->Reason, ExecutionRunReason::InvocationComplete)
        << E->Diagnostic;
    EXPECT_EQ(E->Invocation->Value, 69u);
  }
}
TEST_F(DriverModuleInput, ReplacingOriginalDriverDoesNotReplaceThePinnedPlan) {
  auto Source = prepare("driver", "loader");
  ASSERT_TRUE(Source);
  auto Before = loadDriverImage(*Source, MemoryLimit, 0x190000000);
  ASSERT_TRUE(bool(Before));
  std::filesystem::copy_file(fixture("success"), Source->sourcePath(),
                             std::filesystem::copy_options::overwrite_existing);
  auto After = loadDriverImage(*Source, MemoryLimit, 0x190000000);
  ASSERT_TRUE(bool(After));
  same(*Before, *After);
  auto File = loadDriverImage(Source->sourcePath(), MemoryLimit, 0x190000000);
  EXPECT_FALSE(bool(File));
  if (!File)
    EXPECT_NE(llvm::toString(File.takeError()).find("relocation"),
              std::string::npos);
}
TEST_F(DriverModuleInput, FileAndPinnedPlansShareSupportedDataImportContract) {
  auto Source = prepare("driver", "driver_kernel_reference");
  ASSERT_TRUE(Source);
  auto File = loadDriverImage(Source->sourcePath(), MemoryLimit);
  auto Pinned = loadDriverImage(*Source, MemoryLimit);
  ASSERT_TRUE(bool(File)) << llvm::toString(File.takeError());
  ASSERT_TRUE(bool(Pinned)) << llvm::toString(Pinned.takeError());
  same(*File, *Pinned);
}
TEST_F(DriverModuleInput, KernelReferenceConsumesPinnedBytesAndMatchingPdb) {
  auto Source = prepare("kernel", "driver_kernel_debugger_reference", true);
  ASSERT_TRUE(Source);
  auto File = KernelReferenceImage::load(Source->sourcePath(), MemoryLimit,
                                         Source->debugSourcePath());
  ASSERT_TRUE(bool(File)) << llvm::toString(File.takeError());
  const auto Bytes = Source->image().Raw;
  ASSERT_TRUE(std::filesystem::remove(Source->sourcePath()));
  ASSERT_TRUE(std::filesystem::remove(Source->debugSourcePath()));
  Sources.reset();
  auto Pinned = KernelReferenceImage::fromModule(*Source, MemoryLimit);
  ASSERT_TRUE(bool(Pinned)) << llvm::toString(Pinned.takeError());
  EXPECT_EQ(Pinned->exports(), File->exports());
  EXPECT_EQ(Pinned->exportData().names(), File->exportData().names());
  EXPECT_EQ(Pinned->digest(), Source->imageDigest());
  EXPECT_EQ(Pinned->pdbDigest(), Source->debugDigest());
  EXPECT_EQ(Pinned->digest(), File->digest());
  EXPECT_EQ(Pinned->pdbDigest(), File->pdbDigest());
  ASSERT_TRUE(Pinned->debugger());
  EXPECT_EQ(Pinned->debugger()->Template, File->debugger()->Template);
  EXPECT_EQ(Pinned->debugger()->ProcessSize, File->debugger()->ProcessSize);
  EXPECT_EQ(Pinned->debugger()->ThreadSize, File->debugger()->ThreadSize);
  EXPECT_EQ(Source->image().Raw, Bytes);
  // RSDS name trust and original-byte layout proofs remain distinct from
  // general signature/object-size trust, which this provider does not grant.
  EXPECT_FALSE(Source->hasAuthenticatedFunctionSignatures());
  EXPECT_FALSE(Source->hasAuthenticatedObjectExtents());
}
TEST_F(DriverModuleInput, LatePdbDoesNotRetroactivelyChangeKernelFacts) {
  auto Source = prepare("kernel", "driver_kernel_debugger_reference");
  ASSERT_TRUE(Source);
  auto Before = KernelReferenceImage::fromModule(*Source, MemoryLimit);
  ASSERT_TRUE(bool(Before));
  EXPECT_FALSE(Before->debugger());
  auto Debug = Root / "late.pdb";
  std::filesystem::copy_file(
      fixture("driver_kernel_debugger_reference", ".pdb"), Debug);
  auto NewSource = Sources->attachDebug("kernel", {m::DebugKind::PDB, Debug});
  ASSERT_TRUE(bool(NewSource)) << llvm::toString(NewSource.takeError());
  auto After = KernelReferenceImage::fromModule(**NewSource, MemoryLimit);
  ASSERT_TRUE(bool(After)) << llvm::toString(After.takeError());
  EXPECT_TRUE(After->debugger());
  EXPECT_FALSE(Before->debugger());
  EXPECT_EQ(Source->debug(), nullptr);
  EXPECT_NE(*NewSource, Source);
  EXPECT_EQ((*NewSource)->imageDigest(), Source->imageDigest());
}
TEST_F(DriverModuleInput, NamesOnlyMapCannotSupplyKernelLayoutFacts) {
  auto Source = prepare("kernel", "driver_kernel_reference");
  ASSERT_TRUE(Source);
  const auto &Image = Source->image();
  const auto Address = Image.Segments.front().VA;
  auto Map = Root / "reference.map";
  std::ofstream Output(Map);
  Output << " Preferred load address is " << llvm::utohexstr(Image.Base) << "\n"
         << " Address Publics by Value Rva+Base Lib:Object\n\n"
         << " 0001:00000000 mapped_reference " << llvm::utohexstr(Address)
         << " f reference.obj\n";
  Output.close();
  auto Mapped = Sources->attachDebug("kernel", {m::DebugKind::Map, Map});
  ASSERT_TRUE(bool(Mapped)) << llvm::toString(Mapped.takeError());
  auto Reference = KernelReferenceImage::fromModule(**Mapped, MemoryLimit);
  ASSERT_FALSE(bool(Reference));
  EXPECT_NE(llvm::toString(Reference.takeError()).find("authenticated PDB"),
            std::string::npos);
}
} // namespace
