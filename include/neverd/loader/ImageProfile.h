//===- ImageProfile.h - Evidence for an image's target ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Cheap target classification from loader metadata. These are declared build
/// targets, not proof of the privilege level at which arbitrary bytes execute.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_IMAGEPROFILE_H
#define NEVERD_LOADER_IMAGEPROFILE_H

#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <vector>

namespace neverd {

struct BinaryImage;

enum class ImageSystem : uint8_t {
  Unknown,
  Windows,
  WindowsCE,
  Linux,
  Android,
  FreeBSD,
  MacOS,
  IOS,
  OtherDarwin,
  OtherELF,
  UEFI
};
enum class ImageDomain : uint8_t { Unknown, User, Kernel, Firmware };
enum class ImageRuntime : uint8_t { Unknown, GNU, Musl, Bionic };

struct ImageProfile {
  ImageSystem System = ImageSystem::Unknown;
  ImageDomain Domain = ImageDomain::Unknown;
  ImageRuntime Runtime = ImageRuntime::Unknown;
  bool ConflictingEvidence = false;
  std::vector<llvm::StringRef> Evidence;
  std::vector<unsigned> VisualStudioYears;
};

/// Never uses the input filename, entry address, or absence of imports as
/// positive platform evidence. Conflicting declarations remain unknown.
ImageProfile identifyImageProfile(const BinaryImage &Image);
llvm::StringRef imageSystemName(ImageSystem System);
llvm::StringRef imageDomainName(ImageDomain Domain);
llvm::StringRef imageRuntimeName(ImageRuntime Runtime);

} // namespace neverd

#endif // NEVERD_LOADER_IMAGEPROFILE_H
