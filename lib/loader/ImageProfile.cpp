//===- ImageProfile.cpp - Metadata-based image target classification ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Classify declared platforms and execution domains before signature I/O.
//===----------------------------------------------------------------------===//

#include "neverd/loader/ImageProfile.h"

#include "neverd/Limits.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/RichHeader.h"
#include "neverd/loader/ELF/ELFOperatingSystem.h"
#include "neverd/loader/MachO/DarwinOperatingSystem.h"

#include "llvm/ADT/StringSwitch.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/BinaryFormat/MachO.h"

namespace neverd {
namespace {

void evidence(ImageProfile &Profile, llvm::StringRef Reason) {
  Profile.Evidence.push_back(Reason);
}

void classifyPE(const BinaryImage &Image, ImageProfile &Profile) {
  using namespace llvm::COFF;
  if (Image.COFFRichHeader)
    Profile.VisualStudioYears = richToolsetYears(*Image.COFFRichHeader);
  if (!Image.DynInfo.PESubsystem || Image.IsRelocatable)
    return;
  const auto Subsystem = *Image.DynInfo.PESubsystem;
  bool User = false, Kernel = false;
  switch (Subsystem) {
  case IMAGE_SUBSYSTEM_EFI_APPLICATION:
  case IMAGE_SUBSYSTEM_EFI_BOOT_SERVICE_DRIVER:
  case IMAGE_SUBSYSTEM_EFI_RUNTIME_DRIVER:
  case IMAGE_SUBSYSTEM_EFI_ROM:
    Profile.System = ImageSystem::UEFI;
    Profile.Domain = ImageDomain::Firmware;
    evidence(Profile, "pe.efi-subsystem");
    return;
  case IMAGE_SUBSYSTEM_WINDOWS_CE_GUI:
    Profile.System = ImageSystem::WindowsCE;
    evidence(Profile, "pe.windows-ce-subsystem");
    return;
  case IMAGE_SUBSYSTEM_WINDOWS_GUI:
  case IMAGE_SUBSYSTEM_WINDOWS_CUI:
    User = true;
    break;
  case IMAGE_SUBSYSTEM_NATIVE:
    // NATIVE also describes user processes such as smss.exe. The subsystem
    // alone must never turn an ntdll client into a kernel driver.
    Kernel = (Image.DynInfo.PECharacteristics & IMAGE_FILE_SYSTEM) != 0;
    break;
  case IMAGE_SUBSYSTEM_NATIVE_WINDOWS:
    Kernel = true;
    break;
  default:
    return;
  }
  Profile.System = ImageSystem::Windows;
  evidence(Profile, "pe.windows-subsystem");
  if (Image.DynInfo.PECharacteristics & IMAGE_FILE_SYSTEM) {
    Kernel = true;
    evidence(Profile, "pe.system-image-flag");
  }
  if (User)
    evidence(Profile, "pe.user-subsystem");
  bool KernelImports = false, UserImports = false;
  for (const auto &Import : Image.Imports) {
    const llvm::StringRef Module = Import.Module;
    KernelImports |=
        llvm::StringSwitch<bool>(Module)
            .CasesLower({"ntoskrnl.exe", "ntkrnlmp.exe", "hal.dll",
                         "wdfldr.sys", "fltmgr.sys", "wdf01000.sys"},
                        true)
            .Default(false);
    UserImports |= llvm::StringSwitch<bool>(Module)
                       .CasesLower({"kernel32.dll", "kernelbase.dll",
                                    "user32.dll", "ntdll.dll"},
                                   true)
                       .Default(false);
  }
  if (KernelImports)
    evidence(Profile, "pe.kernel-imports");
  if (UserImports)
    evidence(Profile, "pe.user-imports");
  Kernel |= KernelImports;
  User |= UserImports;
  if (Kernel && User) {
    Profile.ConflictingEvidence = true;
    return;
  }
  Profile.Domain = Kernel ? ImageDomain::Kernel
                   : User ? ImageDomain::User
                          : ImageDomain::Unknown;
}

bool hasLinuxModuleMetadata(const BinaryImage &Image) {
  if (!Image.ELFMetadata || Image.ELFMetadata->Type != llvm::ELF::ET_REL)
    return false;
  bool Descriptor = false, Version = false;
  for (const auto &Section : Image.Sections) {
    if (Section.Name == ".gnu.linkonce.this_module" &&
        Section.Type == llvm::ELF::SHT_PROGBITS && Section.FileSz != 0 &&
        Section.isWritable())
      Descriptor = true;
    if (Section.Name != ".modinfo" || Section.Type != llvm::ELF::SHT_PROGBITS ||
        Section.Data.empty() ||
        Section.Data.size() > limits::kMaxImageProfileMetadataBytes)
      continue;
    llvm::StringRef Text(reinterpret_cast<const char *>(Section.Data.data()),
                         Section.Data.size());
    while (!Text.empty()) {
      const size_t End = Text.find('\0');
      if (End == llvm::StringRef::npos)
        break;
      llvm::StringRef Record = Text.take_front(End);
      Version |= Record.consume_front("vermagic=") && !Record.empty();
      Text = Text.drop_front(End + 1);
    }
  }
  return Descriptor && Version;
}

void classifyELF(const BinaryImage &Image, ImageProfile &Profile) {
  if (!Image.ELFMetadata)
    return;
  const auto &Metadata = *Image.ELFMetadata;
  const auto Interpreter = elfInterpreterLibC(Metadata.Interpreter);
  const bool Module = hasLinuxModuleMetadata(Image);
  const bool Android =
      Metadata.AndroidApiLevel.has_value() || Interpreter == LinuxLibC::Bionic;
  const auto GNUOS = [&](GNUABISystem System) {
    return Metadata.GNUABISystem == static_cast<uint32_t>(System);
  };
  const bool FreeBSD = runsUnderFreeBSD(Image) || GNUOS(GNUABISystem::FreeBSD);
  // Processor-specific and unassigned OSABI bytes are not evidence of an
  // incompatible OS (ELFOSABI_ARM, for example, does not exclude Linux).
  bool Other = GNUOS(GNUABISystem::Hurd) || GNUOS(GNUABISystem::Solaris) ||
               GNUOS(GNUABISystem::NetBSD) || GNUOS(GNUABISystem::Syllable) ||
               GNUOS(GNUABISystem::NaCl);
  switch (Metadata.OSABI) {
  case llvm::ELF::ELFOSABI_HPUX:
  case llvm::ELF::ELFOSABI_NETBSD:
  case llvm::ELF::ELFOSABI_HURD:
  case llvm::ELF::ELFOSABI_SOLARIS:
  case llvm::ELF::ELFOSABI_AIX:
  case llvm::ELF::ELFOSABI_IRIX:
  case llvm::ELF::ELFOSABI_TRU64:
  case llvm::ELF::ELFOSABI_OPENBSD:
  case llvm::ELF::ELFOSABI_OPENVMS:
  case llvm::ELF::ELFOSABI_STANDALONE:
    Other = true;
    break;
  default:
    break;
  }
  bool GNU = Interpreter == LinuxLibC::GNU;
  for (const auto &Library : Image.DynInfo.NeededLibs)
    GNU |= Library == "libc.so.6";
  const bool Musl = Interpreter == LinuxLibC::Musl;
  const bool Linux = Metadata.OSABI == llvm::ELF::ELFOSABI_GNU ||
                     GNUOS(GNUABISystem::Linux) || Android || GNU || Musl ||
                     Module;
  Profile.ConflictingEvidence =
      Metadata.ConflictingGNUABISystems || (FreeBSD && Linux) ||
      (Other && (FreeBSD || Linux)) || (Android && (GNU || Musl)) ||
      (GNU && Musl) ||
      (Module && (!Metadata.Interpreter.empty() || GNU || Musl || Android));
  Profile.System = Android   ? ImageSystem::Android
                   : FreeBSD ? ImageSystem::FreeBSD
                   : Other   ? ImageSystem::OtherELF
                   : Linux   ? ImageSystem::Linux
                             : ImageSystem::Unknown;
  Profile.Runtime = Android ? ImageRuntime::Bionic
                    : Musl  ? ImageRuntime::Musl
                    : GNU   ? ImageRuntime::GNU
                            : ImageRuntime::Unknown;
  if (Module) {
    Profile.Domain = ImageDomain::Kernel;
    evidence(Profile, "elf.linux-module-descriptor-and-vermagic");
  } else if (Metadata.Type != llvm::ELF::ET_REL &&
             (!Metadata.Interpreter.empty() || Android || GNU || Musl)) {
    Profile.Domain = ImageDomain::User;
  }
  if (Metadata.OSABI != llvm::ELF::ELFOSABI_NONE)
    evidence(Profile, "elf.osabi");
  if (Metadata.GNUABISystem)
    evidence(Profile, "elf.gnu-abi-tag");
  if (Metadata.FreeBSDVersion)
    evidence(Profile, "elf.freebsd-abi-tag");
  if (Metadata.AndroidApiLevel)
    evidence(Profile, "elf.android-ident");
  if (Interpreter)
    evidence(Profile, "elf.runtime-interpreter");
  if (GNU && !Interpreter)
    evidence(Profile, "elf.gnu-libc-dependency");
}

void classifyMachO(const BinaryImage &Image, ImageProfile &Profile) {
  const auto &Platforms = Image.DynInfo.MachOPlatforms;
  if (Platforms.size() > 1) {
    Profile.ConflictingEvidence = true;
    evidence(Profile, "macho.multiple-build-platforms");
    return;
  }
  if (auto Platform = darwinPlatform(Image))
    Profile.System = *Platform == DarwinPlatform::MacOS ? ImageSystem::MacOS
                                                        : ImageSystem::IOS;
  else if (!Platforms.empty())
    Profile.System = ImageSystem::OtherDarwin;
  if (!Platforms.empty())
    evidence(Profile, "macho.build-platform");
  if (!Image.DynInfo.MachOFileType)
    return;
  switch (*Image.DynInfo.MachOFileType) {
  case llvm::MachO::MH_KEXT_BUNDLE:
    Profile.Domain = ImageDomain::Kernel;
    evidence(Profile, "macho.kernel-extension");
    break;
  case llvm::MachO::MH_DYLIB:
  case llvm::MachO::MH_DYLINKER:
  case llvm::MachO::MH_BUNDLE:
    Profile.Domain = ImageDomain::User;
    evidence(Profile, "macho.user-library-type");
    break;
  case llvm::MachO::MH_EXECUTE:
    // XNU kernels are also MH_EXECUTE. A dependency on the process runtime
    // supplies the additional user-space evidence.
    for (const auto &Library : Image.DynInfo.NeededLibs)
      if (Library == "/usr/lib/libSystem.B.dylib") {
        Profile.Domain = ImageDomain::User;
        evidence(Profile, "macho.process-runtime");
      }
    break;
  default:
    break;
  }
}

} // namespace

ImageProfile identifyImageProfile(const BinaryImage &Image) {
  ImageProfile Profile;
  if (Image.isCOFF())
    classifyPE(Image, Profile);
  else if (Image.isELF())
    classifyELF(Image, Profile);
  else if (Image.isMachO())
    classifyMachO(Image, Profile);
  if (Profile.ConflictingEvidence) {
    Profile.System = ImageSystem::Unknown;
    Profile.Domain = ImageDomain::Unknown;
    Profile.Runtime = ImageRuntime::Unknown;
    evidence(Profile, "target.conflicting-metadata");
  }
  return Profile;
}

llvm::StringRef imageSystemName(ImageSystem System) {
  switch (System) {
  case ImageSystem::Unknown:
    return "unknown";
  case ImageSystem::Windows:
    return "windows";
  case ImageSystem::WindowsCE:
    return "windows-ce";
  case ImageSystem::Linux:
    return "linux";
  case ImageSystem::Android:
    return "android";
  case ImageSystem::FreeBSD:
    return "freebsd";
  case ImageSystem::MacOS:
    return "macos";
  case ImageSystem::IOS:
    return "ios";
  case ImageSystem::OtherDarwin:
    return "other-darwin";
  case ImageSystem::OtherELF:
    return "other-elf";
  case ImageSystem::UEFI:
    return "uefi";
  }
  llvm_unreachable("invalid image system");
}

llvm::StringRef imageDomainName(ImageDomain Domain) {
  switch (Domain) {
  case ImageDomain::Unknown:
    return "unknown";
  case ImageDomain::User:
    return "user";
  case ImageDomain::Kernel:
    return "kernel";
  case ImageDomain::Firmware:
    return "firmware";
  }
  llvm_unreachable("invalid image domain");
}

llvm::StringRef imageRuntimeName(ImageRuntime Runtime) {
  switch (Runtime) {
  case ImageRuntime::Unknown:
    return "unknown";
  case ImageRuntime::GNU:
    return "glibc";
  case ImageRuntime::Musl:
    return "musl";
  case ImageRuntime::Bionic:
    return "bionic";
  }
  llvm_unreachable("invalid image runtime");
}

} // namespace neverd
