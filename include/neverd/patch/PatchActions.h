//===- PatchActions.h - Interactive instruction edits ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Architecture-owned patch planning; previews never mutate the image.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_PATCH_PATCHACTIONS_H
#define NEVERD_PATCH_PATCHACTIONS_H

#include "neverd/patch/PatchIO.h"

#include <map>

namespace neverd::patch {
struct AssemblyRequest {
  va_t Address = 0;
  InstructionMode Mode = InstructionMode::Default;
  bool ATT = false;
  std::string Text;
  std::map<std::string, int64_t> Symbols;
};
llvm::Expected<std::vector<uint8_t>> assemble(Arch Architecture,
                                              const AssemblyRequest &Request);

struct PatchRequest {
  std::map<std::string, int64_t> Symbols;
  va_t Address = 0;
  PatchKind Kind = PatchKind::Bytes;
  std::vector<uint8_t> Bytes;
  std::string Text;
  uint64_t Value = 0;
  uint64_t Size = 0;
  bool FillNops = true;
  bool AllowOverlap = false;
  bool ATT = false;
};
struct PatchPlan {
  va_t Address = 0;
  uint64_t FileOffset = 0;
  PatchKind Kind = PatchKind::Bytes;
  std::vector<uint8_t> Before, After;
  llvm::json::Array Covers, Decoded, Warnings;
  bool CoversTruncated = false;
  bool DecodedTruncated = false;
  llvm::json::Object json() const;
};
llvm::Expected<PatchPlan> planPatch(const BinaryImage &Img,
                                    const InputFile &Input,
                                    const PatchOverlay &Overlay,
                                    const PatchRequest &Request);
} // namespace neverd::patch
#endif // NEVERD_PATCH_PATCHACTIONS_H
