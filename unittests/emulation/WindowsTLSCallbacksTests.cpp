//===- WindowsTLSCallbacksTests.cpp - Byte-addressed TLS callback arrays --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checks original PE callbacks at every array alignment against execution.
//===----------------------------------------------------------------------===//

#include "WindowsNativeTestSupport.h"
#include "gtest/gtest.h"
#include "os/windows/process/WindowsProcessModules.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>

namespace neverd::emulation {
namespace {
namespace win = windows_process;
using namespace llvm::object;
constexpr uint64_t Base = 0x140000000;
constexpr uint32_t Code = 0x1000, Data = 0x2000, Directory = 0x2040;
constexpr uint32_t Marker = 0x2100, Index = 0x2120, Result = 42;

// A leaf executable whose TLS callback writes Result and whose entry reads it.
// It has no imports, CRT, toolchain dependency or private binary bytes.
std::string callbackImage(GuestArchitecture ISA, unsigned ArrayOffset) {
  std::string Bytes(0x600, '\0');
  auto Store = [&](uint64_t Offset, const auto &Record) {
    std::memcpy(Bytes.data() + Offset, &Record, sizeof(Record));
  };
  dos_header DOS{};
  DOS.Magic[0] = 'M';
  DOS.Magic[1] = 'Z';
  DOS.AddressOfNewExeHeader = 0x80;
  Store(0, DOS);
  std::memcpy(Bytes.data() + 0x80, llvm::COFF::PEMagic,
              sizeof(llvm::COFF::PEMagic));
  coff_file_header Header{};
  Header.Machine = ISA == GuestArchitecture::X64
                       ? llvm::COFF::IMAGE_FILE_MACHINE_AMD64
                       : llvm::COFF::IMAGE_FILE_MACHINE_ARM64;
  Header.NumberOfSections = 2;
  Header.SizeOfOptionalHeader =
      sizeof(pe32plus_header) + 16 * sizeof(data_directory);
  Header.Characteristics = llvm::COFF::IMAGE_FILE_EXECUTABLE_IMAGE |
                           llvm::COFF::IMAGE_FILE_LARGE_ADDRESS_AWARE |
                           llvm::COFF::IMAGE_FILE_RELOCS_STRIPPED;
  Store(0x84, Header);
  pe32plus_header PE{};
  PE.Magic = llvm::COFF::PE32Header::PE32_PLUS;
  PE.SizeOfCode = 0x200;
  PE.SizeOfInitializedData = 0x200;
  PE.AddressOfEntryPoint = Code + 0x20;
  PE.BaseOfCode = Code;
  PE.ImageBase = Base;
  PE.SectionAlignment = 0x1000;
  PE.FileAlignment = 0x200;
  PE.MajorOperatingSystemVersion = 6;
  PE.MajorSubsystemVersion = 6;
  PE.SizeOfImage = 0x3000;
  PE.SizeOfHeaders = 0x200;
  PE.Subsystem = llvm::COFF::IMAGE_SUBSYSTEM_WINDOWS_CUI;
  PE.SizeOfStackReserve = 0x100000;
  PE.SizeOfHeapReserve = 0x100000;
  PE.SizeOfStackCommit = 0x1000;
  PE.SizeOfHeapCommit = 0x1000;
  PE.NumberOfRvaAndSize = 16;
  Store(0x98, PE);
  data_directory TLSDirectory{};
  TLSDirectory.RelativeVirtualAddress = Directory;
  TLSDirectory.Size = sizeof(coff_tls_directory64);
  Store(0x98 + sizeof(PE) + llvm::COFF::TLS_TABLE * sizeof(data_directory),
        TLSDirectory);
  for (unsigned I = 0; I < 2; ++I) {
    coff_section Section{};
    std::memcpy(Section.Name, I ? ".data" : ".text", 5);
    Section.VirtualSize = 0x1000;
    Section.VirtualAddress = I ? Data : Code;
    Section.SizeOfRawData = 0x200;
    Section.PointerToRawData = 0x200 * (I + 1);
    Section.Characteristics =
        llvm::COFF::IMAGE_SCN_MEM_READ |
        (I ? llvm::COFF::IMAGE_SCN_MEM_WRITE |
                 llvm::COFF::IMAGE_SCN_CNT_INITIALIZED_DATA
           : llvm::COFF::IMAGE_SCN_MEM_EXECUTE |
                 llvm::COFF::IMAGE_SCN_CNT_CODE);
    Store(0x188 + I * sizeof(Section), Section);
  }
  coff_tls_directory64 TLS{};
  TLS.AddressOfIndex = Base + Index;
  TLS.AddressOfCallBacks = Base + Data + ArrayOffset;
  Store(0x440, TLS);
  llvm::support::endian::write64le(Bytes.data() + 0x400 + ArrayOffset,
                                   Base + Code);
  if (ISA == GuestArchitecture::X64) {
    // mov dword ptr [rip+Marker], Result; ret
    const std::array<uint8_t, 11> Callback{0xc7,   0x05, 0xf6, 0x10, 0,   0,
                                           Result, 0,    0,    0,    0xc3};
    // mov eax, dword ptr [rip+Marker]; ret
    const std::array<uint8_t, 7> Entry{0x8b, 0x05, 0xda, 0x10, 0, 0, 0xc3};
    std::memcpy(Bytes.data() + 0x200, Callback.data(), Callback.size());
    std::memcpy(Bytes.data() + 0x220, Entry.data(), Entry.size());
  } else {
    // adrp x9, Data; mov w8, Result; str w8,[x9,#256]; ret
    const std::array<uint32_t, 4> Callback{0xb0000009, 0x52800548, 0xb9010128,
                                           0xd65f03c0};
    // adrp x9, Data; ldr w0,[x9,#256]; ret
    const std::array<uint32_t, 3> Entry{0xb0000009, 0xb9410120, 0xd65f03c0};
    for (unsigned I = 0; I < Callback.size(); ++I)
      llvm::support::endian::write32le(Bytes.data() + 0x200 + I * 4,
                                       Callback[I]);
    for (unsigned I = 0; I < Entry.size(); ++I)
      llvm::support::endian::write32le(Bytes.data() + 0x220 + I * 4, Entry[I]);
  }
  return Bytes;
}

class WindowsTLSCallbacks : public testing::Test {
protected:
  std::filesystem::path Root;
  void SetUp() override {
    llvm::SmallString<128> Path;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-tls-array", Path));
    Root = Path.str().str();
  }
  void TearDown() override {
    if (!Root.empty())
      std::filesystem::remove_all(Root);
  }
  std::filesystem::path write(const std::string &Bytes) {
    const auto Path = Root / "callbacks.exe";
    std::error_code Error;
    llvm::raw_fd_ostream Output(Path.string(), Error);
    EXPECT_FALSE(Error);
    Output << Bytes;
    Output.close();
    return Path;
  }
};

TEST_F(WindowsTLSCallbacks, AdmitsEveryByteAlignmentWithoutChangingTheArray) {
  for (auto ISA : {GuestArchitecture::X64, GuestArchitecture::AArch64})
    for (unsigned Offset = 0; Offset < 8; ++Offset) {
      SCOPED_TRACE(static_cast<unsigned>(ISA));
      SCOPED_TRACE(Offset);
      const auto Bytes = callbackImage(ISA, Offset);
      auto Image = win::loadImage(write(Bytes), process_defaults::Memory);
      ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
      EXPECT_EQ(Image->TLSCallbackPointer,
                Base + Directory +
                    offsetof(coff_tls_directory64, AddressOfCallBacks));
      EXPECT_EQ(Image->Regions.back().Bytes[Offset], Bytes[0x400 + Offset]);
      EXPECT_EQ(llvm::support::endian::read64le(
                    Image->Regions.back().Bytes.data() + Offset),
                Base + Code);
    }
}

TEST_F(WindowsTLSCallbacks, KeepsArrayBoundsAndExecutableTargetChecks) {
  for (auto ISA : {GuestArchitecture::X64, GuestArchitecture::AArch64})
    for (unsigned Variant = 0; Variant < 5; ++Variant) {
      SCOPED_TRACE(static_cast<unsigned>(ISA));
      SCOPED_TRACE(Variant);
      auto Bytes = callbackImage(ISA, 1);
      if (Variant < 3) {
        const uint64_t Address = Variant == 0   ? Base - 1
                                 : Variant == 1 ? Base + 0x3000 - 7
                                                : Base + 0x2201;
        llvm::support::endian::write64le(
            Bytes.data() + 0x440 +
                offsetof(coff_tls_directory64, AddressOfCallBacks),
            Address);
      } else {
        llvm::support::endian::write64le(
            Bytes.data() + 0x401, Variant == 3 ? Base + Marker : Base + 0x3000);
      }
      auto Image = win::loadImage(write(Bytes), process_defaults::Memory);
      EXPECT_FALSE(bool(Image));
      if (!Image)
        llvm::consumeError(Image.takeError());
    }
  auto Bytes = callbackImage(GuestArchitecture::AArch64, 1);
  llvm::support::endian::write64le(Bytes.data() + 0x401, Base + Code + 1);
  auto Image = win::loadImage(write(Bytes), process_defaults::Memory);
  EXPECT_FALSE(bool(Image));
  if (!Image)
    llvm::consumeError(Image.takeError());
}

class WindowsTLSCallbackExecution
    : public WindowsTLSCallbacks,
      public testing::WithParamInterface<GuestArchitecture> {};

TEST_P(WindowsTLSCallbackExecution, ExecutesOriginalUnalignedArrays) {
  const auto ISA = GetParam();
  ExecutionConfiguration Configuration;
  Configuration.Backend = ExecutionBackendKind::Unicorn;
  Configuration.Architecture = ISA;
  Configuration.Contract = ISA == GuestArchitecture::X64
                               ? ExecutionContract::CheckedUserX64
                               : ExecutionContract::CheckedUserAArch64;
  auto Probe = probeExecutionBackend(Configuration);
  ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
  if (Probe->Availability != BackendAvailability::Available)
    GTEST_SKIP() << Probe->Reason;
  for (unsigned Offset = 0; Offset < 8; ++Offset) {
    SCOPED_TRACE(Offset);
    ProcessOptions Options;
    Options.Backend = Configuration.Backend;
    Options.Contract = Configuration.Contract;
    Options.Limits.Instructions = 1000;
    auto Run = emulateProcess(write(callbackImage(ISA, Offset)),
                              ProcessProfile::WindowsPE64, Options);
    ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
    EXPECT_EQ(Run->Stop, ProcessStopReason::Exited) << Run->Diagnostic;
    EXPECT_EQ(Run->ReturnValue, Result);
    EXPECT_EQ(Run->ExitStatus, Result);
  }
}

INSTANTIATE_TEST_SUITE_P(Architectures, WindowsTLSCallbackExecution,
                         testing::Values(GuestArchitecture::X64,
                                         GuestArchitecture::AArch64));

TEST_F(WindowsTLSCallbacks, NativeWindowsExecutesTheSameOriginalImages) {
#if !defined(_WIN32) || !defined(_M_X64)
  GTEST_SKIP() << "Native x64 Windows oracle requires an x64 Windows host";
#else
  for (unsigned Offset = 0; Offset < 8; ++Offset) {
    SCOPED_TRACE(Offset);
    auto Status = native_test::observeNativeProcess(
        write(callbackImage(GuestArchitecture::X64, Offset)), Root / "stdout",
        Root / "stderr", 10);
    ASSERT_TRUE(bool(Status)) << llvm::toString(Status.takeError());
    EXPECT_EQ(*Status, Result);
  }
#endif
}
} // namespace
} // namespace neverd::emulation
