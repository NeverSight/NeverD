//===- KernelFiniteFileSystemTests.cpp - Native file provider contracts
//---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Native file provider contracts.
///

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/WindowsUnicode.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/KernelObjectTypes.h"
#include "os/windows/kernel/WindowsKernelLayout.h"

#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"

#include <string_view>

namespace neverd::emulation {
namespace {
class KernelFiniteFileSystem : public ::testing::Test {
protected:
  static constexpr uint64_t S = 0x20000000;
  std::unique_ptr<UnicornBackend> Memory;
  DriverResult Result;
  std::unique_ptr<KernelModel> Model;
  void ok(llvm::Error E) {
    if (E)
      ADD_FAILURE() << llvm::toString(std::move(E));
  }
  void SetUp() override {
    DriverOptions O;
    O.FiniteFileSystem = std::vector<DriverFiniteFileSystemVolume>{
        {"C",
         "\\Device\\NeverDVolumeC",
         {{"\\data", true, {}},
          {"\\data\\hello.bin", false, {1, 2, 3, 4, 5}}}}};
    initialize(O);
  }
  void initialize(const DriverOptions &O) {
    Model.reset();
    auto M = UnicornBackend::create(4 * 1024 * 1024);
    ASSERT_TRUE(bool(M)) << llvm::toString(M.takeError());
    Memory = std::move(*M);
    ok(Memory->map(S, 0x10000, Read | Write));
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverImage I;
    I.Base = 0x180000000;
    I.Entry = I.Base + 0x1000;
    I.Size = 0x3000;
    ok(Model->initialize(I, O));
    Model->enterForeground();
    Model->enterExecution(profile::StackBase);
  }
  uint64_t call(llvm::StringRef Name, std::initializer_list<uint64_t> A) {
    auto V = Model->call(Name.str(), A);
    if (!V) {
      ADD_FAILURE() << Name.str() << ": " << llvm::toString(V.takeError());
      return UINT64_MAX;
    }
    return *V;
  }
  uint64_t get(uint64_t A, unsigned Width = 8) {
    auto V = Memory->readInteger(A, Width);
    if (!V) {
      ADD_FAILURE() << llvm::toString(V.takeError());
      return 0;
    }
    return *V;
  }
  void put(uint64_t A, uint64_t V, unsigned Width = 8) {
    ok(Memory->writeInteger(A, V, Width));
  }
  void name(llvm::StringRef Text, uint64_t Root = 0) {
    auto Bytes = windows_unicode::bytes(Text, true);
    ASSERT_TRUE(bool(Bytes)) << llvm::toString(Bytes.takeError());
    ok(Memory->write(S + 0x200, *Bytes));
    put(S + 0x100, Bytes->size() - 2, 2);
    put(S + 0x102, Bytes->size(), 2);
    put(S + 0x108, S + 0x200);
    put(S + 0x300, 48, 4);
    put(S + 0x308, Root);
    put(S + 0x310, S + 0x100);
    put(S + 0x318, 0x240, 4);
  }
  uint64_t open(llvm::StringRef Text = "\\DosDevices\\C:",
                uint32_t Access = 0x100000, uint32_t Options = 0x21,
                uint32_t Share = 1, uint64_t Root = 0) {
    name(Text, Root);
    return call("ZwCreateFile", {S, Access, S + 0x300, S + 0x400, 0, 0, Share,
                                 1, Options, 0, 0});
  }
  uint64_t reference(uint64_t Handle) {
    EXPECT_EQ(
        call("ObReferenceObjectByHandle", {Handle, 0, 0, 0, S + 0x500, 0}), 0u);
    return get(S + 0x500);
  }
  void error(llvm::StringRef Name, std::initializer_list<uint64_t> A,
             llvm::StringRef Text) {
    auto V = Model->call(Name.str(), A);
    ASSERT_FALSE(bool(V));
    EXPECT_NE(llvm::toString(V.takeError()).find(Text.str()),
              std::string::npos);
  }
};

class KernelMutableFiniteFileSystem : public KernelFiniteFileSystem {
protected:
  void SetUp() override {
    DriverOptions O;
    O.FiniteFileSystem = std::vector<DriverFiniteFileSystemVolume>{
        {"C",
         "\\Device\\NeverDVolumeC",
         {{"\\data", true, {}},
          {"\\data\\hello.bin", false, {1, 2, 3, 4, 5}},
          {"\\empty", true, {}}},
         false,
         {{"\\SystemRoot", "\\data"}, {"\\VolumeRoot", "\\"}}}};
    initialize(O);
  }
  uint64_t create(llvm::StringRef Path, uint32_t Disposition = 2,
                  uint32_t Access = 0x130003, uint32_t Options = 0x60,
                  uint32_t Share = 7) {
    name(Path);
    return call("ZwCreateFile", {S, Access, S + 0x300, S + 0x400, 0, 0x80,
                                 Share, Disposition, Options, 0, 0});
  }
  uint64_t info(uint64_t H, uint64_t Value, uint32_t Class) {
    put(S + 0x900, Value);
    return call("ZwSetInformationFile",
                {H, S + 0x400, S + 0x900, Class == 13 ? 1u : 8u, Class});
  }
  uint64_t write(uint64_t H, uint64_t Offset, uint32_t Length = 2) {
    put(S + 0x900, Offset);
    put(S + 0x1000, 0x5958, 2);
    return call("ZwWriteFile",
                {H, 0, 0, 0, S + 0x400, S + 0x1000, Length, S + 0x900, 0});
  }
};

TEST_F(KernelFiniteFileSystem,
       BaseFiniteFileSystemDeviceUsesFileIdentityAndSurvivesHandleClose) {
  ASSERT_EQ(open("\\??\\C:\\data\\hello.bin", 0x120089, 0x60, 7), 0u);
  const auto First = get(S), FirstObject = reference(First);
  const auto Device = get(FirstObject + windows::FileDeviceOffset);
  EXPECT_EQ(call("IoGetBaseFileSystemDeviceObject", {FirstObject}), Device);
  const auto H = First, Object = FirstObject;
  EXPECT_EQ(call("IoGetBaseFileSystemDeviceObject", {Object}), Device);
  EXPECT_EQ(call("ZwClose", {H}), 0u);
  call("KfRaiseIrql", {2});
  EXPECT_EQ(call("IoGetBaseFileSystemDeviceObject", {Object}), Device);
  call("KeLowerIrql", {0});
  EXPECT_EQ(call("ObfDereferenceObject", {Object}), 0u);
  error("IoGetBaseFileSystemDeviceObject", {Object}, "FILE_OBJECT identity");
}

TEST_F(KernelFiniteFileSystem,
       BaseFiniteFileSystemDeviceRejectsUnprovidedVPBAndChangedIdentity) {
  ASSERT_EQ(open("\\??\\C:\\data\\hello.bin", 0x120089, 0x60, 7), 0u);
  const auto H = get(S), Object = reference(H),
             Device = get(Object + windows::FileDeviceOffset);
  put(Object + windows::FileVPBOffset, S + 0x800);
  error("IoGetBaseFileSystemDeviceObject", {Object}, "VPB provider");
  put(Object + windows::FileVPBOffset, 0);
  put(Object + windows::FileDeviceOffset, S + 0x800);
  error("IoGetBaseFileSystemDeviceObject", {Object}, "changed identity");
  put(Object + windows::FileDeviceOffset, Device);
  EXPECT_EQ(call("ZwClose", {H}), 0u);
  EXPECT_EQ(call("ObfDereferenceObject", {Object}), 0u);
}

TEST_F(KernelFiniteFileSystem,
       IoCreateFileSharesExistingBytesAndNativeFileObjects) {
  EXPECT_EQ(Model->argumentCount("IoCreateFile"), 14u);
  for (uint64_t Flags : {0ULL, 0x100ULL, 0xabcdef0000000100ULL}) {
    name("\\??\\C:\\data\\hello.bin");
    ASSERT_EQ(call("IoCreateFile", {S, 0x80000000, S + 0x300, S + 0x400, 0,
                                    0x80, 7, 1, 0x60, 0, 0, 0, 0, Flags}),
              0u);
    const auto H = get(S);
    EXPECT_EQ(get(S + 0x408), 1u);
    ASSERT_EQ(call("ZwReadFile", {H, 0, 0, 0, S + 0x400, S + 0x800, 5, 0, 0}),
              0u);
    EXPECT_EQ(get(S + 0x408), 5u);
    EXPECT_EQ(get(S + 0x800, 4), 0x04030201u);
    EXPECT_EQ(get(S + 0x804, 1), 5u);
    const auto F = reference(H);
    EXPECT_EQ(get(F, 2), 5u);
    EXPECT_EQ(call("ZwClose", {H}), 0u);
    EXPECT_EQ(call("ObfDereferenceObject", {F}), 0u);
  }
}

TEST_F(KernelMutableFiniteFileSystem,
       IoCreateFileSharesCreationAndDeleteLifetime) {
  name("\\SystemRoot\\iocreated.bin");
  ASSERT_EQ(call("IoCreateFile", {S, 0x130003, S + 0x300, S + 0x400, 0, 0x80, 7,
                                  2, 0x60, 0, 0, 0, 0, 0x100}),
            0u);
  const auto H = get(S);
  EXPECT_EQ(get(S + 0x408), 2u);
  EXPECT_EQ(write(H, 0), 0u);
  EXPECT_EQ(create("\\??\\C:\\data\\iocreated.bin"), 0xc0000035u);
  EXPECT_EQ(call("ZwDeleteFile", {S + 0x300}), 0u);
  EXPECT_EQ(open("\\SystemRoot\\iocreated.bin", 0x120089, 0x60, 7),
            0xc0000056u);
  EXPECT_EQ(call("ZwClose", {H}), 0u);
  EXPECT_EQ(open("\\SystemRoot\\iocreated.bin", 0x120089, 0x60, 7),
            0xc0000034u);
}

TEST_F(KernelFiniteFileSystem,
       DeviceHintSharesVolumeAndForcesKernelParameterMode) {
  EXPECT_EQ(Model->argumentCount("IoCreateFileSpecifyDeviceObjectHint"), 15u);
  ASSERT_EQ(open("\\??\\C:\\data\\hello.bin", 0x120089, 0x60, 7), 0u);
  const auto Original = get(S), Object = reference(Original);
  const auto Device = get(Object + windows::FileDeviceOffset);
  ok(Model->setUserRequestContext(true, 100));
  for (const uint64_t Hint : {uint64_t(0), Device}) {
    name("\\??\\C:\\data\\hello.bin");
    ASSERT_EQ(call("IoCreateFileSpecifyDeviceObjectHint",
                   {S, 0x80000000, S + 0x300, S + 0x400, 0, 0x80, 7, 1, 0x60, 0,
                    0, 0, 0, 0, Hint}),
              0u);
    const auto H = get(S);
    EXPECT_EQ(get(S + 0x408), 1u);
    ASSERT_EQ(call("ZwReadFile", {H, 0, 0, 0, S + 0x400, S + 0x800, 5, 0, 0}),
              0u);
    EXPECT_EQ(get(S + 0x800, 4), 0x04030201u);
    EXPECT_EQ(call("ZwClose", {H}), 0u);
  }
  ok(Model->setUserRequestContext(false));
  EXPECT_EQ(call("ZwClose", {Original}), 0u);
  EXPECT_EQ(call("ObfDereferenceObject", {Object}), 0u);
}

TEST_F(KernelMutableFiniteFileSystem,
       DeviceHintRejectsOtherVolumesBeforeCreation) {
  DriverOptions O;
  O.FiniteFileSystem = std::vector<DriverFiniteFileSystemVolume>{
      {"C", "\\Device\\NeverDVolumeC", {{"\\data", true, {}}}, false},
      {"D", "\\Device\\NeverDVolumeD", {{"\\data", true, {}}}, false}};
  initialize(O);
  ASSERT_EQ(open("\\??\\D:", 0x100000, 0x21, 7), 0u);
  const auto H = get(S), Object = reference(H);
  const auto OtherDevice = get(Object + windows::FileDeviceOffset);
  name("\\??\\C:\\data\\never-created.bin");
  put(S, 0x11112222);
  put(S + 0x400, 0x33334444);
  put(S + 0x408, 0x55556666);
  EXPECT_EQ(call("IoCreateFileSpecifyDeviceObjectHint",
                 {S, 0x130003, S + 0x300, S + 0x400, 0, 0x80, 7, 2, 0x60, 0, 0,
                  0, 0, 0, OtherDevice}),
            0xc0000369u);
  EXPECT_EQ(get(S), 0x11112222u);
  EXPECT_EQ(get(S + 0x400), 0x33334444u);
  EXPECT_EQ(get(S + 0x408), 0x55556666u);
  EXPECT_EQ(open("\\??\\C:\\data\\never-created.bin", 0x120089, 0x60, 7),
            0xc0000034u);
  EXPECT_EQ(call("ZwClose", {H}), 0u);
  EXPECT_EQ(call("ObfDereferenceObject", {Object}), 0u);
}

TEST_F(KernelMutableFiniteFileSystem,
       IoCreateFileRejectsExtraPoliciesBeforeOutputsOrNamespaceChange) {
  name("\\SystemRoot\\never-created.bin");
  put(S, 0x11112222);
  put(S + 0x400, 0x33334444);
  put(S + 0x408, 0x55556666);
  for (unsigned Case = 0; Case < 4; ++Case) {
    error("IoCreateFile",
          {S, 0x130003, S + 0x300, S + 0x400, 0, 0x80, 7, 2, 0x60, 0, 0,
           Case == 0 ? 1u : 0u, Case == 1 ? S + 0x800 : 0,
           Case == 2   ? 1u
           : Case == 3 ? 0x800u
                       : 0u},
          Case < 2 ? "CreateFileTypeNone" : "unmodeled");
    EXPECT_EQ(get(S), 0x11112222u);
    EXPECT_EQ(get(S + 0x400), 0x33334444u);
    EXPECT_EQ(get(S + 0x408), 0x55556666u);
  }
  EXPECT_EQ(open("\\SystemRoot\\never-created.bin", 0x120089, 0x60, 7),
            0xc0000034u);
}

TEST_F(KernelFiniteFileSystem,
       IoCreateFileRequiresExplicitParameterModeAndPassiveIRQL) {
  name("\\??\\C:\\data\\hello.bin");
  ok(Model->setUserRequestContext(true, 100));
  error(
      "IoCreateFile",
      {S, 0x80000000, S + 0x300, S + 0x400, 0, 0x80, 7, 1, 0x60, 0, 0, 0, 0, 0},
      "user-mode parameter probing");
  ASSERT_EQ(call("IoCreateFile", {S, 0x80000000, S + 0x300, S + 0x400, 0, 0x80,
                                  7, 1, 0x60, 0, 0, 0, 0, 0x100}),
            0u);
  EXPECT_EQ(call("ZwClose", {get(S)}), 0u);
  ok(Model->setUserRequestContext(false));
  call("KfRaiseIrql", {1});
  error("IoCreateFile",
        {S, 0x80000000, S + 0x300, S + 0x400, 0, 0x80, 7, 1, 0x60, 0, 0, 0, 0,
         0x100},
        "IRQL");
  call("KeLowerIrql", {0});
}

TEST_F(KernelMutableFiniteFileSystem,
       RootAliasesShareNamespaceAndProtectProviderBindings) {
  ASSERT_EQ(open("\\SystemRoot\\hello.bin", 0x120089, 0x60, 7), 0u);
  EXPECT_EQ(call("ZwClose", {get(S)}), 0u);
  ASSERT_EQ(open("\\VolumeRoot\\data\\hello.bin", 0x120089, 0x60, 7), 0u);
  EXPECT_EQ(call("ZwClose", {get(S)}), 0u);
  name("\\SystemRoot");
  error("IoDeleteSymbolicLink", {S + 0x100}, "provider-owned");
  name("\\SystemRoot\\absent");
  EXPECT_EQ(call("ZwDeleteFile", {S + 0x300}), 0xc0000034u);
  name("\\SystemRoot\\hello.bin");
  EXPECT_EQ(call("ZwDeleteFile", {S + 0x300}), 0u);
  EXPECT_EQ(open("\\??\\C:\\data\\hello.bin", 0x120089, 0x60, 7), 0xc0000034u);
}

TEST_F(KernelMutableFiniteFileSystem,
       CreateDispositionMatrixMatchesNativeOutputs) {
  for (bool Existing : {false, true})
    for (uint32_t D = 0; D <= 5; ++D) {
      SCOPED_TRACE(std::to_string(Existing) + ":" + std::to_string(D));
      const auto Path = "\\??\\C:\\data\\matrix" + std::to_string(Existing) +
                        std::to_string(D);
      if (Existing) {
        ASSERT_EQ(create(Path), 0u);
        ASSERT_EQ(call("ZwClose", {get(S)}), 0u);
      }
      put(S, 0xabababab);
      put(S + 0x400, 0xa5a5a5a5a5a5a5a5);
      put(S + 0x408, 0xcccccccc);
      const auto Expected = !Existing && (D == 1 || D == 4) ? 0xc0000034u
                            : Existing && D == 2            ? 0xc0000035u
                                                            : 0u;
      EXPECT_EQ(create(Path, D), Expected);
      if (Expected) {
        EXPECT_EQ(get(S), 0xababababu);
        EXPECT_EQ(get(S + 0x400), 0xa5a5a5a5a5a5a5a5u);
        EXPECT_EQ(get(S + 0x408), 0xccccccccu);
      } else {
        EXPECT_EQ(get(S + 0x400), 0xa5a5a5a500000000u);
        EXPECT_EQ(get(S + 0x408), !Existing ? 2u
                                  : D == 0  ? 0u
                                  : D >= 4  ? 3u
                                            : 1u);
        EXPECT_EQ(call("ZwClose", {get(S)}), 0u);
      }
    }
}

TEST_F(KernelMutableFiniteFileSystem,
       SharedDataOffsetsTruncationAndFinalSnapshot) {
  ASSERT_EQ(create("\\SystemRoot\\hello.bin", 1), 0u);
  const auto H = get(S);
  ASSERT_EQ(open("\\??\\C:\\data\\hello.bin", 0x120089, 0x60, 7), 0u);
  const auto Reader = get(S);
  const auto F = reference(H);
  EXPECT_EQ(write(H, 7), 0u);
  EXPECT_EQ(get(F + windows::FileCurrentByteOffset), 9u);
  EXPECT_EQ(write(H, 100, 0), 0u);
  EXPECT_EQ(get(F + windows::FileCurrentByteOffset), 9u);
  EXPECT_EQ(write(H, uint64_t(-1), 1), 0u);
  EXPECT_EQ(write(H, uint64_t(-2), 1), 0u);
  EXPECT_EQ(info(H, 2, 14), 0u);
  EXPECT_EQ(info(H, 4, 20), 0u);
  EXPECT_EQ(info(H, 7, 20), 0u);
  EXPECT_EQ(get(F + windows::FileCurrentByteOffset), 2u);
  EXPECT_EQ(
      call("ZwReadFile", {Reader, 0, 0, 0, S + 0x400, S + 0x1100, 8, 0, 0}),
      0u);
  EXPECT_EQ(get(S + 0x408), 7u);
  EXPECT_EQ(get(S + 0x1100, 4), 0x04030201u);
  EXPECT_EQ(get(S + 0x1104, 2), 0u);
  EXPECT_EQ(get(S + 0x1106, 1), 0u);
  EXPECT_EQ(call("ZwQueryInformationFile", {H, S + 0x400, S + 0x1200, 24, 5}),
            0u);
  EXPECT_EQ(get(S + 0x1200), 8u);
  EXPECT_EQ(get(S + 0x1208), 7u);
  EXPECT_EQ(get(S + 0x1210, 4), 1u);
  EXPECT_EQ(call("ZwFlushBuffersFile", {H, S + 0x400}), 0u);
  ok(Model->snapshot());
  ASSERT_TRUE(Result.FiniteFileSystem);
  const auto &Entries = Result.FiniteFileSystem->at(0).Entries;
  const auto It =
      std::find_if(Entries.begin(), Entries.end(),
                   [](const auto &E) { return E.Path == "\\data\\hello.bin"; });
  ASSERT_NE(It, Entries.end());
  EXPECT_EQ(It->Data, (std::vector<uint8_t>{1, 2, 3, 4, 0, 0, 0}));
  EXPECT_EQ(call("ZwClose", {Reader}), 0u);
  EXPECT_EQ(call("ZwClose", {H}), 0u);
  EXPECT_EQ(call("ObfDereferenceObject", {F}), 0u);
}

TEST_F(KernelMutableFiniteFileSystem,
       DeletionSharingPendingAndRetainedObjectGeneration) {
  ASSERT_EQ(open("\\SystemRoot\\hello.bin", 0x120089, 0x60, 1), 0u);
  const auto Unshared = get(S);
  name("\\SystemRoot\\hello.bin");
  EXPECT_EQ(call("ZwDeleteFile", {S + 0x300}), 0xc0000043u);
  EXPECT_EQ(call("ZwClose", {Unshared}), 0u);
  ASSERT_EQ(create("\\SystemRoot\\hello.bin", 1), 0u);
  const auto H = get(S);
  const auto Old = reference(H);
  EXPECT_EQ(get(Old + windows::FileDeleteAccessOffset, 1), 1u);
  EXPECT_EQ(get(Old + windows::FileSharedDeleteOffset, 1), 1u);
  EXPECT_EQ(info(H, 1, 13), 0u);
  EXPECT_EQ(get(Old + windows::FileDeletePendingOffset, 1), 1u);
  EXPECT_EQ(open("\\SystemRoot\\hello.bin", 0x120089, 0x60, 7), 0xc0000056u);
  EXPECT_EQ(info(H, 0, 13), 0u);
  EXPECT_EQ(get(Old + windows::FileDeletePendingOffset, 1), 0u);
  name("\\SystemRoot\\hello.bin");
  EXPECT_EQ(call("ZwDeleteFile", {S + 0x300}), 0u);
  EXPECT_EQ(call("ZwClose", {H}), 0u);
  ASSERT_EQ(create("\\SystemRoot\\hello.bin"), 0u);
  const auto NewHandle = get(S);
  const auto New = reference(NewHandle);
  EXPECT_NE(New, Old);
  EXPECT_EQ(get(New + windows::FileDeletePendingOffset, 1), 0u);
  EXPECT_EQ(call("ObfDereferenceObject", {Old}), 0u);
  EXPECT_EQ(call("ZwClose", {NewHandle}), 0u);
  EXPECT_EQ(call("ObfDereferenceObject", {New}), 0u);
}

TEST_F(KernelMutableFiniteFileSystem, DeleteOnCloseAndDirectoryCleanup) {
  ASSERT_EQ(create("\\SystemRoot\\temporary", 2, 0x130003, 0x1060), 0u);
  EXPECT_EQ(call("ZwClose", {get(S)}), 0u);
  EXPECT_EQ(open("\\SystemRoot\\temporary", 0x120089, 0x60, 7), 0xc0000034u);
  name("\\??\\C:\\data");
  EXPECT_EQ(call("ZwDeleteFile", {S + 0x300}), 0u);
  ASSERT_EQ(open("\\??\\C:\\data", 0x110000, 0x21, 7), 0u);
  const auto Dir = get(S);
  EXPECT_EQ(info(Dir, 1, 13), 0xc0000101u);
  EXPECT_EQ(call("ZwClose", {Dir}), 0u);
  name("\\??\\C:\\empty");
  EXPECT_EQ(call("ZwDeleteFile", {S + 0x300}), 0u);
  EXPECT_EQ(open("\\??\\C:\\empty"), 0xc0000034u);
}

TEST_F(KernelMutableFiniteFileSystem,
       OutputPreflightAndCapacityFailuresDoNotMutateData) {
  ASSERT_EQ(create("\\SystemRoot\\hello.bin", 1), 0u);
  const auto H = get(S);
  put(S + 0x900, 0);
  put(S + 0x1000, 0x5858, 2);
  auto V =
      Model->call("ZwWriteFile", {H, 0, 0, 0, 0, S + 0x1000, 2, S + 0x900, 0});
  ASSERT_FALSE(bool(V));
  llvm::consumeError(V.takeError());
  EXPECT_EQ(info(H, MaxDriverFiniteFileSystemBytes + 1, 20), 0xc000007fu);
  EXPECT_EQ(write(H, MaxDriverFiniteFileSystemBytes, 1), 0xc000007fu);
  EXPECT_EQ(call("ZwQueryInformationFile", {H, S + 0x400, S + 0x1200, 24, 5}),
            0u);
  EXPECT_EQ(get(S + 0x1208), 5u);
  error("ZwSetInformationFile", {H, S + 0x400, S + 0x900, 8, 99},
        "information class");
  EXPECT_EQ(call("ZwClose", {H}), 0u);
}

TEST_F(KernelMutableFiniteFileSystem,
       DeleteOnCloseRemainsArmedAfterDispositionCancellation) {
  ASSERT_EQ(create("\\SystemRoot\\temporary", 2, 0x130003, 0x1060), 0u);
  const auto H = get(S);
  EXPECT_EQ(info(H, 0, 13), 0u);
  EXPECT_EQ(call("ZwClose", {H}), 0u);
  EXPECT_EQ(open("\\SystemRoot\\temporary", 0x120089, 0x60, 7), 0xc0000034u);
}

TEST_F(KernelMutableFiniteFileSystem,
       NamespaceExpansionRejectsCyclesBeforeFileMutation) {
  auto Link = [&](llvm::StringRef Name, llvm::StringRef Target) {
    name(Name);
    put(S + 0x700, Target.size() * 2, 2);
    put(S + 0x702, (Target.size() + 1) * 2, 2);
    put(S + 0x708, S + 0x800);
    for (size_t I = 0; I <= Target.size(); ++I)
      put(S + 0x800 + I * 2, I == Target.size() ? 0 : uint8_t(Target[I]), 2);
    EXPECT_EQ(call("IoCreateSymbolicLink", {S + 0x100, S + 0x700}), 0u);
  };
  Link("\\LoopA", "\\LoopB");
  Link("\\LoopB", "\\LoopA");
  name("\\LoopA\\file");
  error("ZwCreateFile",
        {S, 0x130003, S + 0x300, S + 0x400, 0, 0, 7, 2, 0x60, 0, 0},
        "symbolic-link cycle");
}

TEST(DriverFiniteFileSystemScenario,
     MutableAliasesRoundTripAndRejectMalformedBindings) {
  const std::string Prefix =
      R"({"finite_filesystem":[{"drive":"C","device_name":"\\Device\\C","entries":[{"path":"\\Windows","directory":true,"data":""}],)";
  const std::string Alias =
      R"("read_only":false,"aliases":[{"name":"\\SystemRoot","path":"\\Windows"}]}]})";
  auto O = driverOptionsFromScenarioJSON(Prefix + Alias);
  ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
  EXPECT_FALSE(O->FiniteFileSystem->at(0).ReadOnly);
  EXPECT_EQ(O->FiniteFileSystem->at(0).Aliases.at(0).Path, "\\Windows");
  DriverResult R;
  R.Configuration = *O;
  R.FiniteFileSystem = O->FiniteFileSystem;
  R.FiniteFileSystem->at(0).Entries.push_back(
      {"\\Windows\\created", false, {0x41}});
  auto J = llvm::json::parse(driverResultJSON(R));
  ASSERT_TRUE(bool(J));
  const auto *Initial = J->getAsObject()
                            ->getObject("configuration")
                            ->getArray("finite_filesystem");
  const auto *Final = J->getAsObject()->getArray("finite_filesystem");
  ASSERT_TRUE(Final);
  EXPECT_EQ((*Initial)[0].getAsObject()->getArray("entries")->size(), 1u);
  EXPECT_EQ((*Final)[0].getAsObject()->getArray("entries")->size(), 2u);
  for (
      const char *Tail :
      {R"("read_only":0}]})", R"("aliases":null}]})",
       R"("aliases":[{"name":"\\SystemRoot","path":"\\absent"}]}]})",
       R"("aliases":[{"name":"\\Device","path":"\\Windows"}]}]})",
       R"("aliases":[{"name":"\\Driver","path":"\\Windows"}]}]})",
       R"("aliases":[{"name":"\\SystemRoot","path":"\\Windows"},{"name":"\\systemroot","path":"\\"}]}]})"}) {
    auto Bad = driverOptionsFromScenarioJSON(Prefix + Tail);
    ASSERT_FALSE(bool(Bad)) << Tail;
    llvm::consumeError(Bad.takeError());
  }
}

TEST_F(KernelFiniteFileSystem, ReadOnlyVolumesRejectMutations) {
  EXPECT_EQ(open("\\??\\C:\\data\\hello.bin", 0x130003, 0x60, 7), 0xc00000a2u);
  name("\\??\\C:\\data\\hello.bin");
  EXPECT_EQ(call("ZwDeleteFile", {S + 0x300}), 0xc00000a2u);
}

TEST_F(KernelFiniteFileSystem,
       DriveEnumerationMatchesNativeOpenOutputsAndFileIdentity) {
  put(S, 0x77777777);
  put(S + 0x400, 0xa5a5a5a5a5a5a5a5);
  put(S + 0x408, 0xabababababababab);
  EXPECT_EQ(open("\\DosDevices\\A:"), 0xc0000034u);
  EXPECT_EQ(get(S), 0x77777777u);
  EXPECT_EQ(get(S + 0x400), 0xa5a5a5a5a5a5a5a5u);
  EXPECT_EQ(get(S + 0x408), 0xababababababababu);
  EXPECT_EQ(open("\\??\\C:\\missing\\file"), 0xc000003au);
  ASSERT_EQ(open(), 0u);
  const auto H = get(S);
  EXPECT_EQ(get(S + 0x400), 0xa5a5a5a500000000u);
  EXPECT_EQ(get(S + 0x408), 0u);
  auto F = reference(H);
  EXPECT_EQ(get(F, 2), 5u);
  EXPECT_EQ(get(F + 2, 2), windows::FileSize);
  EXPECT_EQ(get(F + windows::FileFlagsOffset, 4), windows::FileFlags);
  const auto Device = get(F + windows::FileDeviceOffset);
  EXPECT_EQ(get(Device, 2), 3u);
  EXPECT_EQ(get(Device + windows::DeviceTypeOffset, 4), 7u);
  EXPECT_EQ(get(Device + windows::DeviceReferenceCount, 4), 1u);
  ok(Model->validateGuestAccess(Device, 2, false));
  auto Denied =
      Model->validateGuestAccess(Device + windows::DeviceFlagsOffset, 4, true);
  ASSERT_TRUE(bool(Denied));
  llvm::consumeError(std::move(Denied));
  EXPECT_EQ(call("ZwClose", {H}), 0u);
  EXPECT_EQ(get(Device + windows::DeviceReferenceCount, 4), 0u);
  EXPECT_TRUE(get(F + windows::FileFlagsOffset, 4) &
              windows::FileCleanupComplete);
  ok(Model->validateGuestAccess(F, 8, false));
  EXPECT_EQ(call("ObfDereferenceObject", {F}), 0u);
  Denied = Model->validateGuestAccess(F, 2, false);
  ASSERT_TRUE(bool(Denied));
  llvm::consumeError(std::move(Denied));
  EXPECT_EQ(call("ZwClose", {H}), 0xc0000008u);
}

TEST_F(KernelFiniteFileSystem, RootDirectoryOpenHasFileOpenedInformation) {
  ASSERT_EQ(open("\\??\\C:\\"), 0u);
  EXPECT_EQ(get(S + 0x408), 1u);
  auto H = get(S);
  auto F = reference(H);
  EXPECT_EQ(get(F + windows::FileNameOffset, 2), 2u);
  EXPECT_EQ(get(F + windows::FileFlagsOffset, 4), windows::FileSynchronousIO);
  EXPECT_EQ(get(get(F + windows::FileNameBufferOffset), 2), uint64_t('\\'));
  EXPECT_EQ(call("ObfDereferenceObject", {F}), 0u);
  EXPECT_EQ(call("ZwClose", {H}), 0u);
}

TEST_F(KernelFiniteFileSystem,
       ObjectNameSizingMatchesNativeOracleAndPreservesTail) {
  ASSERT_EQ(open(), 0u);
  auto H = get(S);
  const std::string Expected = "\\Device\\NeverDVolumeC";
  const uint32_t Needed = 16 + (Expected.size() + 1) * 2;
  for (uint32_t Size : {0, 1, 15, 16, 17, 32, 1024}) {
    ok(Memory->write(S + 0x1000, std::vector<uint8_t>(1024, 0xa5)));
    EXPECT_EQ(call("ZwQueryObject", {H, 1, S + 0x1000, Size, S + 0x900}),
              Size < 16       ? 0xc0000004u
              : Size < Needed ? 0x80000005u
                              : 0u);
    EXPECT_EQ(get(S + 0x900, 4), Needed);
    if (Size < Needed)
      EXPECT_EQ(get(S + 0x1000), 0xa5a5a5a5a5a5a5a5u);
    else {
      EXPECT_EQ(get(S + 0x1000, 2), Expected.size() * 2);
      EXPECT_EQ(get(S + 0x1002, 2), Expected.size() * 2 + 2);
      EXPECT_EQ(get(S + 0x1004, 4), 0u);
      EXPECT_EQ(get(S + 0x1008), S + 0x1010);
      EXPECT_EQ(get(S + 0x1000 + Needed), 0xa5a5a5a5a5a5a5a5u);
    }
  }
  EXPECT_EQ(call("ZwClose", {H}), 0u);
}

TEST_F(KernelFiniteFileSystem, AliasedCreateOutputsFollowNativeWriteOrder) {
  name("\\DosDevices\\C:");
  put(S + 0x400, 0xa5a5a5a5a5a5a5a5);
  ASSERT_EQ(call("ZwCreateFile", {S + 0x400, 0x100000, S + 0x300, S + 0x400, 0,
                                  0, 1, 1, 0x21, 0, 0}),
            0u);
  EXPECT_EQ(get(S + 0x400), 0u); // handle 0x50000000, then status DWORD zero
  EXPECT_EQ(get(S + 0x408), 0u);
  EXPECT_EQ(call("ZwClose", {0x50000000}), 0u);
}

TEST_F(KernelFiniteFileSystem, ReadBytesRelativeNamesSharedCursorAndEof) {
  ASSERT_EQ(open("\\??\\C:\\data"), 0u);
  auto Root = get(S);
  ASSERT_EQ(open("hello.bin", 0x120089, 0x60, 1, Root), 0u);
  auto H = get(S);
  auto F = reference(H);
  EXPECT_EQ(call("ZwReadFile", {H, 0, 0, 0, S + 0x400, S + 0x1000, 3, 0, 0}),
            0u);
  EXPECT_EQ(get(S + 0x1000, 2), 0x0201u);
  EXPECT_EQ(get(S + 0x408), 3u);
  EXPECT_EQ(get(F + windows::FileCurrentByteOffset), 3u);
  EXPECT_EQ(call("ZwQueryInformationFile", {H, S + 0x400, S + 0x900, 8, 14}),
            0u);
  EXPECT_EQ(get(S + 0x900), 3u);
  EXPECT_EQ(
      call("ObOpenObjectByPointer", {F, 0x200, 0, 0x120089, 0, 0, S + 0x800}),
      0u);
  auto Duplicate = get(S + 0x800);
  EXPECT_EQ(call("ZwClose", {H}), 0u);
  EXPECT_EQ(
      call("ZwReadFile", {Duplicate, 0, 0, 0, S + 0x400, S + 0x1000, 3, 0, 0}),
      0u);
  EXPECT_EQ(get(S + 0x1000, 2), 0x0504u);
  EXPECT_EQ(get(S + 0x408), 2u);
  EXPECT_EQ(
      call("ZwReadFile", {Duplicate, 0, 0, 0, S + 0x400, S + 0x1000, 3, 0, 0}),
      0xc0000011u);
  EXPECT_EQ(get(S + 0x408), 0u);
  EXPECT_EQ(call("ZwClose", {Duplicate}), 0u);
  EXPECT_EQ(call("ObfDereferenceObject", {F}), 0u);
  EXPECT_EQ(call("ZwClose", {Root}), 0u);
}

TEST_F(KernelFiniteFileSystem,
       SharingHoldEndsAtCleanupWhilePointerRemainsLive) {
  ASSERT_EQ(open("\\??\\C:\\data\\hello.bin", 0x120089, 0x60, 0), 0u);
  auto H = get(S);
  auto F = reference(H);
  EXPECT_EQ(open("\\??\\C:\\data\\hello.bin", 0x120089, 0x60, 1), 0xc0000043u);
  EXPECT_EQ(call("ZwClose", {H}), 0u);
  ASSERT_EQ(open("\\??\\C:\\data\\hello.bin", 0x120089, 0x60, 1), 0u);
  EXPECT_EQ(call("ZwClose", {get(S)}), 0u);
  EXPECT_EQ(call("ObfDereferenceObject", {F}), 0u);
}

TEST_F(KernelFiniteFileSystem, DirectoryAndMissingParentStatuses) {
  EXPECT_EQ(open("\\??\\C:\\data\\hello.bin"), 0xc0000103u);
  EXPECT_EQ(open("\\??\\C:\\data", 0x100000, 0x60), 0xc00000bau);
  EXPECT_EQ(open("\\??\\C:\\data\\missing"), 0xc0000034u);
  EXPECT_EQ(open("\\??\\C:\\missing\\file"), 0xc000003au);
}

TEST_F(KernelFiniteFileSystem, InvalidOutputsDoNotPublishHandlesOrWriteStatus) {
  name("\\DosDevices\\C:");
  put(S + 0x400, 0x12345678);
  error("ZwCreateFile",
        {0xdead0000, 0x100000, S + 0x300, S + 0x400, 0, 0, 1, 1, 0x21, 0, 0},
        "permissions");
  EXPECT_EQ(get(S + 0x400), 0x12345678u);
  EXPECT_EQ(call("ZwClose", {0x50000000}), 0xc0000008u);
  ASSERT_EQ(open(), 0u);
  EXPECT_EQ(get(S), 0x50000000u);
  EXPECT_EQ(call("ZwClose", {get(S)}), 0u);
}

TEST_F(KernelFiniteFileSystem,
       UnsupportedOperationsFailClearlyAndObjectTypesAreChecked) {
  ASSERT_EQ(open(), 0u);
  auto H = get(S);
  error("ZwQueryObject", {H, 0, S + 0x1000, 1024, 0}, "information class");
  EXPECT_EQ(call("ObReferenceObjectByHandle",
                 {H, 0, object_type::identity(object_type::Kind::Thread), 0,
                  S + 0x500, 0}),
            0xc0000024u);
  error("ObReferenceObjectByHandle", {H, 0, 0, 1, S + 0x500, 0}, "KernelMode");
  EXPECT_EQ(call("ZwClose", {H}), 0u);
  name("\\DosDevices\\C:");
  error("ZwCreateFile",
        {S, 0x100000, S + 0x300, S + 0x400, 0, 0, 1, 1, 1, 0, 0},
        "synchronous");
  error("ZwCreateFile",
        {S, 0x100000, S + 0x300, S + 0x400, 0, 0, 1, 2, 0x21, 0, 0},
        "raw volume creation");
}

TEST_F(KernelFiniteFileSystem, MissingAliasDoesNotInventAFileSystem) {
  initialize(DriverOptions{});
  name("\\DosDevices\\C:");
  EXPECT_EQ(call("ZwCreateFile",
                 {S, 0x100000, S + 0x300, S + 0x400, 0, 0, 1, 1, 0x21, 0, 0}),
            0xc0000034u);
}

TEST_F(KernelFiniteFileSystem, FileObjectsAndPublishedNamesAreProviderOwned) {
  ASSERT_EQ(open(), 0u);
  auto H = get(S);
  auto F = reference(H);
  for (auto Address : {F + windows::FileContextOffset,
                       get(F + windows::FileNameBufferOffset)}) {
    auto E = Model->validateGuestAccess(Address, 1, true);
    ASSERT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
  }
  // KernelMode references accept specific rights independently of granted
  // rights.
  EXPECT_EQ(call("ObReferenceObjectByHandle", {H, 1, 0, 0, S + 0x500, 0}), 0u);
  EXPECT_EQ(call("ObfDereferenceObject", {F}), 0u);
  EXPECT_EQ(call("ObfDereferenceObject", {F}), 0u);
  EXPECT_EQ(call("ZwClose", {H}), 0u);
}

TEST_F(KernelFiniteFileSystem, ZeroLengthReadAndFailedReadPreserveCursor) {
  ASSERT_EQ(open("\\??\\C:\\data\\hello.bin", 0x120089, 0x60), 0u);
  auto H = get(S);
  auto F = reference(H);
  put(S + 0x900, 2);
  EXPECT_EQ(
      call("ZwReadFile", {H, 0, 0, 0, S + 0x400, S + 0x1000, 1, S + 0x900, 0}),
      0u);
  EXPECT_EQ(get(F + windows::FileCurrentByteOffset), 3u);
  put(S + 0x900, 1000000000);
  EXPECT_EQ(
      call("ZwReadFile", {H, 0, 0, 0, S + 0x400, S + 0x1000, 0, S + 0x900, 0}),
      0u);
  EXPECT_EQ(get(F + windows::FileCurrentByteOffset), 3u);
  EXPECT_EQ(
      call("ZwReadFile", {H, 0, 0, 0, S + 0x400, S + 0x1000, 1, S + 0x900, 0}),
      0xc0000011u);
  EXPECT_EQ(get(F + windows::FileCurrentByteOffset), 3u);
  put(S + 0x900, uint64_t(-1));
  put(S + 0x400, 0xabcddcba);
  EXPECT_EQ(
      call("ZwReadFile", {H, 0, 0, 0, S + 0x400, S + 0x1000, 1, S + 0x900, 0}),
      0xc000000du);
  EXPECT_EQ(get(S + 0x400), 0xabcddcbau);
  EXPECT_EQ(call("ZwClose", {H}), 0u);
  EXPECT_EQ(call("ObfDereferenceObject", {F}), 0u);
}

TEST_F(KernelFiniteFileSystem, OpenFileAndImmutableEmptyNamespace) {
  name("\\??\\C:\\data\\hello.bin");
  EXPECT_EQ(call("ZwOpenFile", {S, 0x80000000, S + 0x300, S + 0x400, 1, 0x60}),
            0u);
  EXPECT_EQ(call("ZwClose", {get(S)}), 0u);
  DriverOptions O;
  O.FiniteFileSystem = std::vector<DriverFiniteFileSystemVolume>{};
  initialize(O);
  EXPECT_EQ(open(), 0xc0000034u);
}

TEST_F(KernelFiniteFileSystem,
       DeviceCaseFoldingAndAliasOwnershipUseSharedNamespace) {
  ASSERT_EQ(open("\\device\\neverdvolumec"), 0u);
  EXPECT_EQ(call("ZwClose", {get(S)}), 0u);
  name("\\DosDevices\\c:");
  error("IoDeleteSymbolicLink", {S + 0x100}, "provider-owned");
  ASSERT_EQ(open(), 0u);
  EXPECT_EQ(call("ZwClose", {get(S)}), 0u);
}

TEST_F(KernelFiniteFileSystem,
       ProcessHandlesAreScopedAndKernelHandlesCrossProcesses) {
  name("\\DosDevices\\C:");
  put(S + 0x318, 0x40, 4);
  ASSERT_EQ(call("ZwOpenFile", {S, 0x100000, S + 0x300, S + 0x400, 1, 0x21}),
            0u);
  auto Scoped = get(S);
  ASSERT_EQ(open(), 0u);
  auto Kernel = get(S);
  ok(Model->setUserRequestContext(true, 0x3456));
  EXPECT_EQ(call("ZwClose", {Scoped}), 0xc0000008u);
  EXPECT_EQ(call("ZwClose", {Kernel}), 0u);
  ok(Model->setUserRequestContext(false));
  EXPECT_EQ(call("ZwClose", {Scoped}), 0u);
}

TEST_F(KernelFiniteFileSystem,
       GuardedRegionCannotIssueFileIoButCriticalRegionCan) {
  EXPECT_EQ(call("KeEnterGuardedRegion", {}), 0u);
  name("\\DosDevices\\C:");
  error("ZwOpenFile", {S, 0x100000, S + 0x300, S + 0x400, 1, 0x21},
        "APCs enabled");
  EXPECT_EQ(call("KeLeaveGuardedRegion", {}), 0u);
  EXPECT_EQ(call("KeEnterCriticalRegion", {}), 0u);
  ASSERT_EQ(open(), 0u);
  EXPECT_EQ(call("ZwClose", {get(S)}), 0u);
  EXPECT_EQ(call("KeLeaveCriticalRegion", {}), 0u);
}

TEST_F(KernelFiniteFileSystem,
       UnloadCanCloseNativeHandlesAndRetiresProviderObjects) {
  ASSERT_EQ(open(), 0u);
  auto H = get(S);
  put(Model->driverObject() + windows::DriverUnloadOffset, 0x180001000);
  ok(Model->finishEntry());
  auto Invocation = Model->beginUnload();
  ASSERT_TRUE(bool(Invocation)) << llvm::toString(Invocation.takeError());
  EXPECT_EQ(call("ZwClose", {H}), 0u);
  ok(Model->finishUnload());
  EXPECT_TRUE(Result.UnloadCompleted);
  EXPECT_TRUE(Result.Devices.empty());
}

TEST(DriverFiniteFileSystemScenario,
     StrictInventoryRoundTripsAndRejectsInvalidTrees) {
  auto O = driverOptionsFromScenarioJSON(
      R"({"finite_filesystem":[{"drive":"C","device_name":"\\Device\\NeverDVolumeC","entries":[{"path":"\\data","directory":true,"data":""},{"path":"\\data\\test","directory":false,"data":"0102FF"}]}]})");
  ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
  ASSERT_TRUE(O->FiniteFileSystem);
  ASSERT_EQ(O->FiniteFileSystem->size(), 1u);
  EXPECT_EQ(O->FiniteFileSystem->at(0).Entries.at(1).Data,
            (std::vector<uint8_t>{1, 2, 255}));
  DriverResult Result;
  Result.Configuration = *O;
  auto Report = llvm::json::parse(driverResultJSON(Result));
  ASSERT_TRUE(bool(Report)) << llvm::toString(Report.takeError());
  const auto *Config = Report->getAsObject()->getObject("configuration");
  ASSERT_TRUE(Config);
  const auto *Inventory = Config->getArray("finite_filesystem");
  ASSERT_TRUE(Inventory);
  ASSERT_EQ(Inventory->size(), 1u);
  EXPECT_EQ((*Inventory)[0].getAsObject()->getString("drive"), "C");
  for (
      const char *Input :
      {R"({"finite_filesystem":null})",
       R"({"finite_filesystem":[{"drive":"c","device_name":"\\Device\\C","entries":[]}]})",
       R"({"finite_filesystem":[{"drive":"C","device_name":"\\Device\\C","typo":1,"entries":[]}]})",
       R"({"finite_filesystem":[{"drive":"C","device_name":"\\Device\\C","entries":[{"path":"\\absent\\file","directory":false,"data":""}]}]})",
       R"({"finite_filesystem":[{"drive":"C","device_name":"\\Device\\C","entries":[{"path":"\\..","directory":false,"data":"00"}]}]})"}) {
    auto Invalid = driverOptionsFromScenarioJSON(Input);
    ASSERT_FALSE(bool(Invalid)) << Input;
    llvm::consumeError(Invalid.takeError());
  }
}
std::string unicodeText(const char8_t *Text) {
  return reinterpret_cast<const char *>(Text);
}
std::vector<uint16_t> volumeUpcase() {
  std::vector<uint16_t> T(65536);
  for (size_t I = 0; I < T.size(); ++I)
    T[I] = uint16_t(I);
  for (unsigned C = 'a'; C <= 'z'; ++C)
    T[C] = C - 32;
  T[0xe9] = 0xc9;
  T[0x3c3] = 0x3a3;
  return T;
}
DriverOptions unicodeVolumeOptions() {
  DriverOptions O;
  DriverFiniteFileSystemVolume V{"C", "\\Device\\NeverDVolumeC", {}, false};
  V.UpcaseTable = volumeUpcase();
  V.Entries = {
      {unicodeText(u8"\\\u6e38\u620f"), true, {}},
      {unicodeText(u8"\\\u6e38\u620f\\\u00c9cole.txt"), false, {1, 2, 3}},
      {unicodeText(u8"\\\u6e38\u620f\\\u03a3.txt"), false, {4}},
      {unicodeText(u8"\\\u6e38\u620f\\\u00df.txt"), false, {5}},
      {unicodeText(u8"\\\u6e38\u620f\\\U0001f600.bin"), false, {6}},
      {unicodeText(u8"\\\u6e38\u620f\\e\u0301cole.txt"), false, {7}}};
  V.Aliases = {{"\\GameRoot", unicodeText(u8"\\\u6e38\u620f")}};
  O.FiniteFileSystem = std::vector<DriverFiniteFileSystemVolume>{std::move(V)};
  return O;
}
class KernelUnicodeFiniteFileSystem : public KernelMutableFiniteFileSystem {
protected:
  void SetUp() override { initialize(unicodeVolumeOptions()); }
  void expectUnits(uint64_t Buffer, std::u16string_view Units) {
    for (size_t I = 0; I < Units.size(); ++I)
      EXPECT_EQ(get(Buffer + I * 2, 2), Units[I]);
  }
};
TEST_F(KernelUnicodeFiniteFileSystem,
       FoldedOpenPreservesExactUTF16FileAndObjectNames) {
  ASSERT_EQ(open(unicodeText(u8"\\??\\C:\\\u6e38\u620f\\\u00e9COLE.TXT"),
                 0x120089, 0x60, 7),
            0u);
  const auto H = get(S), Object = reference(H);
  const std::u16string Relative = u"\\\u6e38\u620f\\\u00c9cole.txt";
  EXPECT_EQ(get(Object + windows::FileNameOffset, 2), Relative.size() * 2);
  EXPECT_EQ(get(Object + windows::FileNameOffset + 2, 2),
            (Relative.size() + 1) * 2);
  expectUnits(get(Object + windows::FileNameBufferOffset), Relative);
  ASSERT_EQ(call("ZwQueryInformationFile", {H, S + 0x400, S + 0x600, 512, 9}),
            0u);
  EXPECT_EQ(get(S + 0x600, 4), Relative.size() * 2);
  EXPECT_EQ(get(S + 0x408), 4 + Relative.size() * 2);
  expectUnits(S + 0x604, Relative);
  const std::u16string Full =
      u"\\Device\\NeverDVolumeC\\\u6e38\u620f\\\u00c9cole.txt";
  ASSERT_EQ(call("ZwQueryObject", {H, 1, S + 0x600, 512, S + 0x900}), 0u);
  EXPECT_EQ(get(S + 0x900, 4), 16 + (Full.size() + 1) * 2);
  EXPECT_EQ(get(S + 0x600, 2), Full.size() * 2);
  expectUnits(get(S + 0x608), Full);
  EXPECT_EQ(call("ObfDereferenceObject", {Object}), 0u);
  EXPECT_EQ(call("ZwClose", {H}), 0u);
}
TEST_F(KernelUnicodeFiniteFileSystem,
       UnicodeAliasesAndRelativeDirectoryOpensUseSameNode) {
  ASSERT_EQ(open("\\GameRoot", 0x120089, 0x21, 7), 0u);
  const auto Dir = get(S);
  ASSERT_EQ(open(unicodeText(u8"\u00e9COLE.TXT"), 0x120089, 0x60, 7, Dir), 0u);
  const auto File = get(S);
  EXPECT_EQ(call("ZwReadFile", {File, 0, 0, 0, S + 0x400, S + 0x600, 3, 0, 0}),
            0u);
  EXPECT_EQ(get(S + 0x600, 1), 1u);
  EXPECT_EQ(get(S + 0x602, 1), 3u);
  EXPECT_EQ(call("ZwClose", {File}), 0u);
  EXPECT_EQ(call("ZwClose", {Dir}), 0u);
  name("\\GameRoot");
  ASSERT_EQ(call("ZwOpenSymbolicLinkObject", {S, 1, S + 0x300}), 0u);
  const auto Link = get(S);
  put(S + 0x702, 512, 2);
  put(S + 0x708, S + 0x800);
  ASSERT_EQ(call("ZwQuerySymbolicLinkObject", {Link, S + 0x700, S + 0x500}),
            0u);
  const std::u16string Target = u"\\Device\\NeverDVolumeC\\\u6e38\u620f";
  EXPECT_EQ(get(S + 0x700, 2), Target.size() * 2);
  EXPECT_EQ(get(S + 0x500, 4), (Target.size() + 1) * 2);
  expectUnits(S + 0x800, Target);
  EXPECT_EQ(call("ZwClose", {Link}), 0u);
}
TEST_F(KernelUnicodeFiniteFileSystem,
       CaseSensitiveOpenAndCreateCollisionAreDistinct) {
  const auto Lower = unicodeText(u8"\\??\\C:\\\u6e38\u620f\\\u00e9cole.txt");
  name(Lower);
  put(S + 0x318, 0x200, 4);
  EXPECT_EQ(call("ZwCreateFile",
                 {S, 0x120089, S + 0x300, S + 0x400, 0, 0, 7, 1, 0x60, 0, 0}),
            0xc0000034u);
  EXPECT_EQ(create(Lower), 0xc0000035u);
  ASSERT_EQ(open(unicodeText(u8"\\??\\C:\\\u6e38\u620f\\\u00c9cole.txt"),
                 0x120089, 0x60, 7),
            0u);
  EXPECT_EQ(call("ZwClose", {get(S)}), 0u);
}
TEST_F(KernelUnicodeFiniteFileSystem,
       MutationDeletionAndRecreationRetainCanonicalKey) {
  const auto Name = unicodeText(u8"\\GameRoot\\\u03c3-new.bin");
  ASSERT_EQ(create(Name), 0u);
  const auto H = get(S);
  EXPECT_EQ(write(H, 0), 0u);
  EXPECT_EQ(info(H, 1, 13), 0u);
  EXPECT_EQ(call("ZwClose", {H}), 0u);
  EXPECT_EQ(open(Name, 0x120089, 0x60, 7), 0xc0000034u);
  ASSERT_EQ(create(unicodeText(u8"\\GameRoot\\\u03a3-NEW.BIN")), 0u);
  EXPECT_EQ(call("ZwClose", {get(S)}), 0u);
  ASSERT_EQ(open("\\GameRoot", 0x130001, 0x21, 7), 0u);
  EXPECT_EQ(info(get(S), 1, 13), 0xc0000101u);
  EXPECT_EQ(call("ZwClose", {get(S)}), 0u);
}
TEST_F(KernelUnicodeFiniteFileSystem,
       SupplementaryNamesAndShortQueriesUseWCHARByteCounts) {
  ASSERT_EQ(
      open(unicodeText(u8"\\GameRoot\\\U0001f600.bin"), 0x120089, 0x60, 7), 0u);
  const auto H = get(S);
  const std::u16string Name = u"\\\u6e38\u620f\\\U0001f600.bin";
  for (uint32_t Small : {4, 5, 6, 7}) {
    put(S + 0x600, 0xaaaaaaaaaaaaaaaa);
    put(S + 0x400, 0xbbbbbbbbbbbbbbbb);
    put(S + 0x408, 0xcccccccccccccccc);
    EXPECT_EQ(
        call("ZwQueryInformationFile", {H, S + 0x400, S + 0x600, Small, 9}),
        0xc0000004u);
    EXPECT_EQ(get(S + 0x600), 0xaaaaaaaaaaaaaaaau);
    EXPECT_EQ(get(S + 0x400), 0xbbbbbbbbbbbbbbbbu);
    EXPECT_EQ(get(S + 0x408), 0xccccccccccccccccu);
  }
  EXPECT_EQ(call("ZwQueryInformationFile", {H, S + 0x400, S + 0x600, 9, 9}),
            0x80000005u);
  EXPECT_EQ(get(S + 0x408), 8u);
  const uint32_t Size = 4 + uint32_t(Name.size() * 2) - 2;
  EXPECT_EQ(call("ZwQueryInformationFile", {H, S + 0x400, S + 0x600, Size, 9}),
            0x80000005u);
  EXPECT_EQ(get(S + 0x600, 4), Name.size() * 2);
  EXPECT_EQ(get(S + 0x408), Size);
  expectUnits(S + 0x604, std::u16string_view(Name).substr(0, Name.size() - 1));
  EXPECT_EQ(call("ZwClose", {H}), 0u);
}
TEST_F(KernelUnicodeFiniteFileSystem,
       NoNormalizationOrMultiCharacterCaseExpansion) {
  for (auto Text :
       {u8"\\GameRoot\\\u00df.txt", u8"\\GameRoot\\e\u0301cole.txt"}) {
    ASSERT_EQ(open(unicodeText(Text), 0x120089, 0x60, 7), 0u);
    EXPECT_EQ(call("ZwClose", {get(S)}), 0u);
  }
  EXPECT_EQ(open("\\GameRoot\\SS.txt", 0x120089, 0x60, 7), 0xc0000034u);
  EXPECT_EQ(open(unicodeText(u8"\\GameRoot\\\u03c2.txt"), 0x120089, 0x60, 7),
            0xc0000034u);
}
TEST_F(KernelUnicodeFiniteFileSystem, UpcasePoliciesAreVolumeOwned) {
  auto O = unicodeVolumeOptions();
  auto V = O.FiniteFileSystem->front();
  V.Drive = "D";
  V.DeviceName = "\\Device\\VolumeD";
  V.Aliases.clear();
  V.UpcaseTable[0x3c3] = 0x3c3;
  V.Entries = {{unicodeText(u8"\\\u03a3.txt"), false, {8}}};
  O.FiniteFileSystem->push_back(V);
  initialize(O);
  ASSERT_EQ(open(unicodeText(u8"\\??\\C:\\\u6e38\u620f\\\u03c3.txt"), 0x120089,
                 0x60, 7),
            0u);
  EXPECT_EQ(call("ZwClose", {get(S)}), 0u);
  EXPECT_EQ(open(unicodeText(u8"\\??\\D:\\\u03c3.txt"), 0x120089, 0x60, 7),
            0xc0000034u);
  ASSERT_EQ(open(unicodeText(u8"\\??\\D:\\\u03a3.txt"), 0x120089, 0x60, 7), 0u);
  EXPECT_EQ(call("ZwClose", {get(S)}), 0u);
}
TEST_F(KernelUnicodeFiniteFileSystem,
       MissingVolumeStatusDoesNotRequireLeafCaseMapping) {
  auto O = unicodeVolumeOptions();
  O.FiniteFileSystem->front().Entries.clear();
  O.FiniteFileSystem->front().Aliases.clear();
  O.FiniteFileSystem->front().UpcaseTable.clear();
  initialize(O);
  put(S, 0xaaaa);
  put(S + 0x400, 0xbbbb);
  put(S + 0x408, 0xcccc);
  EXPECT_EQ(open(unicodeText(u8"\\??\\Z:\\\u7f51\u7edc\u6e38\u620f")),
            0xc000003au);
  EXPECT_EQ(get(S), 0xaaaau);
  EXPECT_EQ(get(S + 0x400), 0xbbbbu);
  EXPECT_EQ(get(S + 0x408), 0xccccu);
  name(unicodeText(u8"\\??\\C:\\\u6e38\u620f"));
  error("ZwCreateFile",
        {S, 0x100000, S + 0x300, S + 0x400, 0, 0, 1, 1, 0x21, 0, 0},
        "upcase_table");
}
TEST_F(KernelUnicodeFiniteFileSystem, MalformedCountedUTF16StopsBeforeOpening) {
  for (uint16_t Unit : {uint16_t(0xd800), uint16_t(0xdc00)}) {
    name("\\??\\C:");
    put(S + 0x100, 2, 2);
    put(S + 0x200, Unit, 2);
    error("ZwCreateFile",
          {S, 0x100000, S + 0x300, S + 0x400, 0, 0, 1, 1, 0x21, 0, 0},
          "UTF-16");
  }
}
TEST(UnicodeVolumeScenario, TablesAndUTF8InventoryRoundTripAndRejectAmbiguity) {
  auto O = unicodeVolumeOptions();
  DriverResult R;
  R.Configuration = O;
  auto Report = llvm::json::parse(driverResultJSON(R));
  ASSERT_TRUE(bool(Report));
  auto *Files = Report->getAsObject()
                    ->getObject("configuration")
                    ->get("finite_filesystem");
  ASSERT_NE(Files, nullptr);
  auto Parsed = driverOptionsFromScenarioJSON(
      "{\"finite_filesystem\":" + llvm::formatv("{0}", *Files).str() + "}");
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  EXPECT_EQ(Parsed->FiniteFileSystem->front().UpcaseTable,
            O.FiniteFileSystem->front().UpcaseTable);
  EXPECT_EQ(Parsed->FiniteFileSystem->front().Entries[0].Path,
            O.FiniteFileSystem->front().Entries[0].Path);
  auto V = O.FiniteFileSystem->front();
  V.Entries.push_back(
      {unicodeText(u8"\\\u6e38\u620f\\\u00e9COLE.TXT"), false, {}});
  auto E = validateDriverFiniteFileSystem(
      std::vector<DriverFiniteFileSystemVolume>{V});
  ASSERT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  for (unsigned Kind = 0; Kind < 4; ++Kind) {
    V = O.FiniteFileSystem->front();
    if (Kind == 0)
      V.UpcaseTable.pop_back();
    if (Kind == 1)
      V.UpcaseTable[0xe9] = 0xd800;
    if (Kind == 2)
      V.UpcaseTable[0xe9] = '\\';
    if (Kind == 3)
      V.UpcaseTable['a'] = 'a';
    E = validateDriverFiniteFileSystem(
        std::vector<DriverFiniteFileSystemVolume>{V});
    ASSERT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
  }
  EXPECT_FALSE(validDriverFiniteFileSystemPath(std::string("\\bad\xc0\xaf")));
  EXPECT_TRUE(validDriverFiniteFileSystemPath("\\" + std::string(239, 'a')));
  EXPECT_FALSE(validDriverFiniteFileSystemPath("\\" + std::string(240, 'a')));
  std::string Long = "\\";
  for (unsigned I = 0; I < 239; ++I)
    Long += unicodeText(u8"\u6e38");
  EXPECT_TRUE(validDriverFiniteFileSystemPath(Long));
  EXPECT_FALSE(validDriverFiniteFileSystemPath(Long + unicodeText(u8"\u6e38")));
}

TEST(UnicodeVolumeDriver, OriginalUTF16DriverOpensMutatesQueriesAndUnloads) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64}) {
    auto O = unicodeVolumeOptions();
    O.Contract = Contract;
    O.Unload = true;
    auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                               "driver_unicode_filesystem.sys",
                           O);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->NTStatus, 0u) << R->Diagnostic;
    EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
    EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
    EXPECT_TRUE(R->Devices.empty());
    auto JSON = llvm::json::parse(driverResultJSON(*R));
    ASSERT_TRUE(bool(JSON));
    EXPECT_EQ(JSON->getAsObject()->getBoolean("scenario_success"), true);
  }
}
TEST(KernelFiniteFileSystemDriver,
     DriverEntryFileBusinessAndUnloadAcrossContracts) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64}) {
    SCOPED_TRACE(executionContractName(Contract));
    DriverOptions O;
    O.Contract = Contract;
    O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
    O.Unload = true;
    O.FiniteFileSystem = std::vector<DriverFiniteFileSystemVolume>{
        {"C",
         "\\Device\\NeverDVolumeC",
         {{"\\data", true, {}},
          {"\\data\\hello.bin", false, {1, 2, 3, 4, 5}}}}};
    O.FiniteFileSystem->at(0).ReadOnly = false;
    O.FiniteFileSystem->at(0).Aliases.push_back({"\\SystemRoot", "\\data"});
    auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                               "driver_filesystem.sys",
                           O);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->NTStatus, 0u) << R->Diagnostic;
    EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
    EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
    auto Report = llvm::json::parse(driverResultJSON(*R));
    ASSERT_TRUE(bool(Report)) << llvm::toString(Report.takeError());
    EXPECT_EQ(Report->getAsObject()->getBoolean("scenario_success"), true);
    EXPECT_TRUE(R->Devices.empty());
    EXPECT_EQ(
        std::count_if(R->Calls.begin(), R->Calls.end(),
                      [](const auto &C) { return C.Name == "ZwCreateFile"; }),
        27);
    EXPECT_EQ(
        std::count_if(R->Calls.begin(), R->Calls.end(),
                      [](const auto &C) { return C.Name == "IoCreateFile"; }),
        2);
    for (const char *API : {"wcscat_s", "wcsrchr", "tolower"})
      EXPECT_EQ(std::count_if(R->Calls.begin(), R->Calls.end(),
                              [&](const auto &C) { return C.Name == API; }),
                1);
    EXPECT_EQ(std::count_if(R->Calls.begin(), R->Calls.end(),
                            [](const auto &C) {
                              return C.Name == "ZwQuerySymbolicLinkObject";
                            }),
              2);
  }
}
} // namespace
} // namespace neverd::emulation
