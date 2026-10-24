//===- ImageProfileTests.cpp - Target evidence and signature admission
//-----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Positive and adversarial metadata controls for automatic pack selection.
//===----------------------------------------------------------------------===//

#include "../loader/PEHeaderFixture.h"
#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFLoader.h"
#include "neverd/loader/ELF/ELFLoader.h"
#include "neverd/loader/ImageProfile.h"
#include "neverd/loader/MachO/MachOLoader.h"
#include "neverd/sigs/SignatureDB.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/MemoryBuffer.h"

#include <filesystem>
#include <vector>

using namespace neverd;

namespace {
using Files = std::vector<std::filesystem::path>;

BinaryImage pe(uint16_t Subsystem) {
  BinaryImage Image;
  Image.Format = BinaryFormat::COFF;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.DynInfo.PESubsystem = Subsystem;
  return Image;
}

BinaryImage elf() {
  BinaryImage Image;
  Image.Format = BinaryFormat::ELF;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.ELFMetadata = ELFImageMetadata{};
  Image.ELFMetadata->Type = llvm::ELF::ET_DYN;
  return Image;
}

TEST(ImageProfile, NativeSubsystemNeedsIndependentPrivilegeEvidence) {
  auto Image = pe(llvm::COFF::IMAGE_SUBSYSTEM_NATIVE);
  EXPECT_EQ(identifyImageProfile(Image).System, ImageSystem::Windows);
  EXPECT_EQ(identifyImageProfile(Image).Domain, ImageDomain::Unknown);
  Image.Imports.push_back({"NTDLL.DLL", "NtClose"});
  EXPECT_EQ(identifyImageProfile(Image).Domain, ImageDomain::User);
  Image.Imports.push_back({"NTOSKRNL.EXE", "ExAllocatePool2"});
  EXPECT_TRUE(identifyImageProfile(Image).ConflictingEvidence);
  EXPECT_EQ(identifyImageProfile(Image).Domain, ImageDomain::Unknown);
  Image.Imports.erase(Image.Imports.begin());
  EXPECT_EQ(identifyImageProfile(Image).Domain, ImageDomain::Kernel);
  Image.Imports.clear();
  Image.DynInfo.PECharacteristics = llvm::COFF::IMAGE_FILE_SYSTEM;
  EXPECT_EQ(identifyImageProfile(Image).Domain, ImageDomain::Kernel);
}

TEST(ImageProfile, COFFObjectsAndFirmwareKeepTheirOwnContracts) {
  auto Image = pe(llvm::COFF::IMAGE_SUBSYSTEM_WINDOWS_CUI);
  EXPECT_EQ(identifyImageProfile(Image).Domain, ImageDomain::User);
  Image.IsRelocatable = true;
  EXPECT_EQ(identifyImageProfile(Image).System, ImageSystem::Unknown);
  Image = pe(llvm::COFF::IMAGE_SUBSYSTEM_EFI_APPLICATION);
  EXPECT_EQ(identifyImageProfile(Image).System, ImageSystem::UEFI);
  EXPECT_EQ(identifyImageProfile(Image).Domain, ImageDomain::Firmware);
  Image = pe(llvm::COFF::IMAGE_SUBSYSTEM_WINDOWS_CE_GUI);
  EXPECT_EQ(identifyImageProfile(Image).System, ImageSystem::WindowsCE);
}

TEST(SignatureSelection, DriverSkipsUIButRetainsMixedWDKAndCompilerHelpers) {
  auto Image = pe(llvm::COFF::IMAGE_SUBSYSTEM_NATIVE);
  Image.Imports.push_back({"ntoskrnl.exe", "ExAllocatePool2"});
  const Files Input = {"winsdk.pat",
                       "supplemental-bds.pat",
                       "supplemental-bds.part2.pat",
                       "supplemental-ms64wdk.pat",
                       "supplemental-msddk64.pat",
                       "supplemental-iclmat64.pat",
                       "supplemental-vc32mfce.pat",
                       "vs2026.pat",
                       "custom.pat"};
  EXPECT_EQ(sigs::SignatureDB::selectForImage(Image, Input),
            (Files{"winsdk.pat", "supplemental-ms64wdk.pat",
                   "supplemental-msddk64.pat", "supplemental-iclmat64.pat",
                   "vs2026.pat", "custom.pat"}));
  Image.DynInfo.PESubsystem = llvm::COFF::IMAGE_SUBSYSTEM_WINDOWS_CUI;
  EXPECT_EQ(sigs::SignatureDB::selectForImage(Image, Input), Input);
}

TEST(SignatureSelection, UserModeKeepsMixedWDKAndUnknownLibraries) {
  const auto Image = pe(llvm::COFF::IMAGE_SUBSYSTEM_WINDOWS_GUI);
  const Files Input = {"supplemental-ms64wdk.pat", "winsdk.pat", "custom.pat",
                       "supplemental-vc32mfce.pat"};
  EXPECT_EQ(sigs::SignatureDB::selectForImage(Image, Input),
            (Files{"supplemental-ms64wdk.pat", "winsdk.pat", "custom.pat"}));
}

TEST(SignatureSelection, MissingMSVCReleaseDoesNotUndoPlatformSelection) {
  auto Image = pe(llvm::COFF::IMAGE_SUBSYSTEM_WINDOWS_CUI);
  Image.COFFRichHeader = RichHeader{};
  Image.COFFRichHeader->ChecksumMatches = true;
  Image.COFFRichHeader->Entries = {{0x0102, 36257, 1}};
  const Files Input = {"vs2017.pat", "vs2022.pat", "supplemental-vc32mfce.pat"};
  EXPECT_EQ(sigs::SignatureDB::selectForImage(Image, Input),
            (Files{"vs2017.pat", "vs2022.pat"}));
}

TEST(SignatureSelection, UnreviewedNamesDoNotInheritAPlatformPrefix) {
  auto Image = elf();
  Image.ELFMetadata->AndroidApiLevel = 28;
  const Files Input = {"ubuntu-libgcc-custom.pat", "ubuntu-libc++-custom.pat"};
  EXPECT_EQ(sigs::SignatureDB::selectForImage(Image, Input), Input);
  Image.Format = BinaryFormat::MachO;
  Image.DynInfo.MachOPlatforms = {llvm::MachO::PLATFORM_IOS};
  EXPECT_EQ(sigs::SignatureDB::selectForImage(Image, {"homebrew-custom.pat"}),
            (Files{"homebrew-custom.pat"}));
}

TEST(ImageProfile, SystemVWithoutRuntimeEvidenceDoesNotMeanLinuxOrUser) {
  auto Image = elf();
  EXPECT_EQ(identifyImageProfile(Image).System, ImageSystem::Unknown);
  EXPECT_EQ(identifyImageProfile(Image).Domain, ImageDomain::Unknown);
  EXPECT_EQ(identifyImageProfile(Image).Runtime, ImageRuntime::Unknown);
  const Files Input = {"android-ndk.pat", "ubuntu-libc6.pat",
                       "ubuntu-musl.pat"};
  EXPECT_EQ(sigs::SignatureDB::selectForImage(Image, Input), Input);
  Image.ELFMetadata->Interpreter = "/system/bin/linker64-unrelated";
  EXPECT_EQ(identifyImageProfile(Image).System, ImageSystem::Unknown);
}

TEST(ImageProfile, ProcessorSpecificOSABIDoesNotExcludeLinux) {
  auto Image = elf();
  Image.Arch = Arch::ARM;
  Image.Bits = Bitness::Bits32;
  Image.ELFMetadata->OSABI = llvm::ELF::ELFOSABI_ARM;
  EXPECT_EQ(identifyImageProfile(Image).System, ImageSystem::Unknown);
  Image.ELFMetadata->Interpreter = "/system/bin/linker";
  EXPECT_EQ(identifyImageProfile(Image).System, ImageSystem::Android);
  EXPECT_FALSE(identifyImageProfile(Image).ConflictingEvidence);
  Image.ELFMetadata->OSABI = llvm::ELF::ELFOSABI_SOLARIS;
  EXPECT_TRUE(identifyImageProfile(Image).ConflictingEvidence);
}

TEST(SignatureSelection, AndroidGlibcAndMuslUseExplicitRuntimeEvidence) {
  auto Image = elf();
  const Files Input = {"android-ndk.pat",      "android-ndk.part2.pat",
                       "ubuntu-libc6.pat",     "ubuntu-libstdc++-12.pat",
                       "ubuntu-musl.pat",      "musl-1.2.5-memory.pat",
                       "ubuntu-libgcc-12.pat", "custom.pat"};
  Image.ELFMetadata->AndroidApiLevel = 28;
  EXPECT_EQ(identifyImageProfile(Image).System, ImageSystem::Android);
  EXPECT_EQ(sigs::SignatureDB::selectForImage(Image, Input),
            (Files{"android-ndk.pat", "android-ndk.part2.pat",
                   "ubuntu-libgcc-12.pat", "custom.pat"}));
  Image.ELFMetadata->AndroidApiLevel.reset();
  Image.ELFMetadata->Interpreter = "/lib64/ld-linux-x86-64.so.2";
  EXPECT_EQ(identifyImageProfile(Image).Runtime, ImageRuntime::GNU);
  EXPECT_EQ(sigs::SignatureDB::selectForImage(Image, Input),
            (Files{"ubuntu-libc6.pat", "ubuntu-libstdc++-12.pat",
                   "ubuntu-libgcc-12.pat", "custom.pat"}));
  Image.ELFMetadata->Interpreter = "/lib/ld-musl-x86_64.so.1";
  EXPECT_EQ(identifyImageProfile(Image).Runtime, ImageRuntime::Musl);
  EXPECT_EQ(sigs::SignatureDB::selectForImage(Image, Input),
            (Files{"ubuntu-musl.pat", "musl-1.2.5-memory.pat",
                   "ubuntu-libgcc-12.pat", "custom.pat"}));
  Image.ELFMetadata->Interpreter = "/apex/com.android.runtime/bin/linker64";
  EXPECT_EQ(identifyImageProfile(Image).System, ImageSystem::Android);
}

TEST(ImageProfile, ELFConflictsAndFreeBSDNeverDefaultToGlibc) {
  auto Image = elf();
  Image.ELFMetadata->FreeBSDVersion = 1400000;
  EXPECT_EQ(identifyImageProfile(Image).System, ImageSystem::FreeBSD);
  EXPECT_EQ(identifyImageProfile(Image).Runtime, ImageRuntime::Unknown);
  Image.ELFMetadata->AndroidApiLevel = 28;
  EXPECT_TRUE(identifyImageProfile(Image).ConflictingEvidence);
  const Files Input = {"android-ndk.pat", "ubuntu-libc6.pat"};
  EXPECT_EQ(sigs::SignatureDB::selectForImage(Image, Input), Input);
  Image = elf();
  Image.ELFMetadata->GNUABISystem = 0;
  EXPECT_EQ(identifyImageProfile(Image).System, ImageSystem::Linux);
  EXPECT_EQ(identifyImageProfile(Image).Runtime, ImageRuntime::Unknown);
  // A Linux kernel ABI note does not distinguish Android from glibc/musl.
  EXPECT_EQ(sigs::SignatureDB::selectForImage(Image, Input), Input);
  Image.ELFMetadata->ConflictingGNUABISystems = true;
  EXPECT_EQ(identifyImageProfile(Image).System, ImageSystem::Unknown);
  Image = elf();
  Image.ELFMetadata->GNUABISystem = 1; // GNU/Hurd also uses glibc.
  Image.DynInfo.NeededLibs = {"libc.so.6"};
  EXPECT_TRUE(identifyImageProfile(Image).ConflictingEvidence);
  EXPECT_EQ(identifyImageProfile(Image).System, ImageSystem::Unknown);
}

TEST(ImageProfile, LinuxModuleNeedsDescriptorAndTerminatedVersion) {
  auto Image = elf();
  Image.ELFMetadata->Type = llvm::ELF::ET_REL;
  Image.IsRelocatable = true;
  Section Descriptor;
  Descriptor.Name = ".gnu.linkonce.this_module";
  Descriptor.Type = llvm::ELF::SHT_PROGBITS;
  Descriptor.FileSz = 128;
  Descriptor.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Image.Sections.push_back(Descriptor);
  EXPECT_EQ(identifyImageProfile(Image).Domain, ImageDomain::Unknown);
  Section Info;
  Info.Name = ".modinfo";
  Info.Type = llvm::ELF::SHT_PROGBITS;
  const std::string Version = "vermagic=6.12.0 SMP";
  Info.Data.assign(Version.begin(), Version.end());
  Image.Sections.push_back(Info);
  EXPECT_EQ(identifyImageProfile(Image).Domain, ImageDomain::Unknown);
  Image.Sections.back().Data.push_back(0);
  EXPECT_EQ(identifyImageProfile(Image).Domain, ImageDomain::Kernel);
  EXPECT_EQ(identifyImageProfile(Image).System, ImageSystem::Linux);
  EXPECT_EQ(sigs::SignatureDB::selectForImage(Image, {"android-ndk.pat",
                                                      "ubuntu-libc6.pat",
                                                      "ubuntu-libgcc-12.pat"}),
            (Files{"ubuntu-libgcc-12.pat"}));
  Image.Sections.back().Data.resize(limits::kMaxImageProfileMetadataBytes + 1);
  EXPECT_EQ(identifyImageProfile(Image).Domain, ImageDomain::Unknown);
}

TEST(SignatureSelection, MachOBuildPlatformAndKextTypeAreIndependent) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.DynInfo.MachOPlatforms = {llvm::MachO::PLATFORM_MACOS};
  Image.DynInfo.MachOFileType = llvm::MachO::MH_DYLIB;
  const Files Input = {"homebrew-zlib.pat", "custom.pat"};
  EXPECT_EQ(sigs::SignatureDB::selectForImage(Image, Input), Input);
  Image.DynInfo.MachOPlatforms = {llvm::MachO::PLATFORM_IOSSIMULATOR};
  EXPECT_EQ(identifyImageProfile(Image).System, ImageSystem::IOS);
  EXPECT_EQ(sigs::SignatureDB::selectForImage(Image, Input),
            (Files{"custom.pat"}));
  Image.DynInfo.MachOPlatforms = {llvm::MachO::PLATFORM_MACOS};
  Image.DynInfo.MachOFileType = llvm::MachO::MH_KEXT_BUNDLE;
  EXPECT_EQ(identifyImageProfile(Image).Domain, ImageDomain::Kernel);
  EXPECT_EQ(sigs::SignatureDB::selectForImage(Image, Input),
            (Files{"custom.pat"}));
  Image.DynInfo.MachOPlatforms.push_back(llvm::MachO::PLATFORM_IOS);
  EXPECT_TRUE(identifyImageProfile(Image).ConflictingEvidence);
  EXPECT_EQ(sigs::SignatureDB::selectForImage(Image, Input), Input);
}

TEST(ImageProfile, RealPEHeadersRetainSubsystemForBothWidths) {
  for (bool Is64 : {false, true}) {
    auto Bytes =
        test::peHeaderImage(Is64, Is64 ? llvm::COFF::IMAGE_FILE_MACHINE_AMD64
                                       : llvm::COFF::IMAGE_FILE_MACHINE_I386);
    auto Headers = locatePEHeaders(reinterpret_cast<uint8_t *>(Bytes.data()),
                                   Bytes.size());
    if (Is64)
      reinterpret_cast<llvm::object::pe32plus_header *>(Headers.OptionalHeader)
          ->Subsystem = llvm::COFF::IMAGE_SUBSYSTEM_NATIVE;
    else
      reinterpret_cast<llvm::object::pe32_header *>(Headers.OptionalHeader)
          ->Subsystem = llvm::COFF::IMAGE_SUBSYSTEM_NATIVE;
    Headers.FileHeader->Characteristics |= llvm::COFF::IMAGE_FILE_SYSTEM;
    COFFLoader Loader;
    auto Image = Loader.loadBuffer(llvm::MemoryBufferRef(Bytes, "renamed.txt"));
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    EXPECT_EQ(identifyImageProfile(*Image).Domain, ImageDomain::Kernel);
    Image->Raw.clear();
    EXPECT_EQ(identifyImageProfile(*Image).Domain, ImageDomain::Kernel);
    Loader.restrictFunctions({Is64 ? 0x140001000ULL : 0x401000ULL});
    Image = Loader.loadBuffer(llvm::MemoryBufferRef(Bytes, "renamed.txt"));
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    EXPECT_TRUE(Image->Raw.empty());
    EXPECT_EQ(identifyImageProfile(*Image).Domain, ImageDomain::Kernel);
  }
}

TEST(ImageProfile, RealELFProgramNotesRetainTheGNUTarget) {
  using web::test::nativePut;
  auto Bytes = web::test::nativeELF();
  nativePut(Bytes, 56, 2, 2); // two program headers
  nativePut(Bytes, 120, llvm::ELF::PT_NOTE, 4);
  nativePut(Bytes, 128, 352, 8);
  nativePut(Bytes, 152, 32, 8);
  nativePut(Bytes, 352, 4, 4);
  nativePut(Bytes, 356, 16, 4);
  nativePut(Bytes, 360, llvm::ELF::NT_GNU_ABI_TAG, 4);
  Bytes.replace(364, 4, "GNU\0", 4);
  nativePut(Bytes, 368, 0, 4);
  ELFLoader Loader;
  auto Image = Loader.loadBuffer(llvm::MemoryBufferRef(Bytes, "renamed.sys"));
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  EXPECT_EQ(identifyImageProfile(*Image).System, ImageSystem::Linux);
  EXPECT_EQ(identifyImageProfile(*Image).Domain, ImageDomain::Unknown);
  // Conflicting notes cannot be resolved by whichever appears last.
  Bytes.replace(384, 32, Bytes.substr(352, 32));
  nativePut(Bytes, 400, 3, 4);
  nativePut(Bytes, 152, 64, 8);
  Image = Loader.loadBuffer(llvm::MemoryBufferRef(Bytes, "conflicting-notes"));
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  EXPECT_TRUE(identifyImageProfile(*Image).ConflictingEvidence);
}

TEST(ImageProfile, RealELFInterpreterIsValidatedBeforeClassification) {
  using web::test::nativePut;
  auto Bytes = web::test::nativeELF();
  const std::string Interpreter = "/system/bin/linker64";
  nativePut(Bytes, 56, 2, 2);
  nativePut(Bytes, 120, llvm::ELF::PT_INTERP, 4);
  nativePut(Bytes, 128, 352, 8);
  nativePut(Bytes, 152, Interpreter.size() + 1, 8);
  Bytes.replace(352, Interpreter.size(), Interpreter);
  ELFLoader Loader;
  auto Image = Loader.loadBuffer(llvm::MemoryBufferRef(Bytes, "android.exe"));
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  EXPECT_EQ(identifyImageProfile(*Image).System, ImageSystem::Android);
  EXPECT_EQ(identifyImageProfile(*Image).Domain, ImageDomain::User);
  nativePut(Bytes, 152, Interpreter.size(), 8);
  Image = Loader.loadBuffer(llvm::MemoryBufferRef(Bytes, "unterminated"));
  ASSERT_FALSE(bool(Image));
  EXPECT_NE(llvm::toString(Image.takeError()).find("PT_INTERP"),
            std::string::npos);
  nativePut(Bytes, 152, Interpreter.size() + 1, 8);
  Bytes[353] = '\0';
  Image = Loader.loadBuffer(llvm::MemoryBufferRef(Bytes, "embedded-null"));
  ASSERT_FALSE(bool(Image));
  EXPECT_NE(llvm::toString(Image.takeError()).find("PT_INTERP"),
            std::string::npos);
  Bytes.replace(352, Interpreter.size(), Interpreter);
  nativePut(Bytes, 56, 3, 2);
  Bytes.replace(176, 56, Bytes.substr(120, 56));
  Image = Loader.loadBuffer(llvm::MemoryBufferRef(Bytes, "duplicate-interp"));
  ASSERT_FALSE(bool(Image));
  EXPECT_NE(llvm::toString(Image.takeError()).find("PT_INTERP"),
            std::string::npos);
  nativePut(Bytes, 56, 2, 2);
  nativePut(Bytes, 152, 1, 8);
  Bytes[352] = '\0';
  Image = Loader.loadBuffer(llvm::MemoryBufferRef(Bytes, "empty-interp"));
  ASSERT_FALSE(bool(Image));
  EXPECT_NE(llvm::toString(Image.takeError()).find("PT_INTERP"),
            std::string::npos);
}

TEST(ImageProfile, RealMachOHeadersRetainBuildPlatformAndKernelType) {
  using web::test::nativePut;
  auto Bytes = web::test::nativeMachO();
  nativePut(Bytes, 12, llvm::MachO::MH_KEXT_BUNDLE, 4);
  // Replace LC_MAIN with the same-sized build version command.
  nativePut(Bytes, 184, llvm::MachO::LC_BUILD_VERSION, 4);
  nativePut(Bytes, 192, llvm::MachO::PLATFORM_MACOS, 4);
  nativePut(Bytes, 196, 0x000d0000, 4);
  MachOLoader Loader;
  auto Image = Loader.loadBuffer(llvm::MemoryBufferRef(Bytes, "renamed.bin"));
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  EXPECT_EQ(identifyImageProfile(*Image).System, ImageSystem::MacOS);
  EXPECT_EQ(identifyImageProfile(*Image).Domain, ImageDomain::Kernel);
  nativePut(Bytes, 12, llvm::MachO::MH_DYLIB, 4);
  nativePut(Bytes, 192, llvm::MachO::PLATFORM_IOS, 4);
  Image = Loader.loadBuffer(llvm::MemoryBufferRef(Bytes, "renamed.bin"));
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  EXPECT_EQ(identifyImageProfile(*Image).System, ImageSystem::IOS);
  EXPECT_EQ(identifyImageProfile(*Image).Domain, ImageDomain::User);
}

} // namespace
