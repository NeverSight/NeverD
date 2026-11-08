//===- MachOAuthenticatedFixups.h - Dyld recipes -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Install loader-owned authenticated pointer storage in a linked image.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_CODEGEN_MACHO_MACHOAUTHENTICATEDFIXUPS_H
#define NEVERD_BACKEND_CODEGEN_MACHO_MACHOAUTHENTICATEDFIXUPS_H

#include "neverd/loader/BinaryImageModel.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <vector>

namespace neverd {

struct CompiledImage;

struct MachOAuthenticatedFixup {
  va_t Address = 0;
  MachOAuthenticatedPointer Recipe;
};

/// Translate producer receipts at their final section VAs. External symbols
/// must identify one exact existing dyld provider; conflicting providers are
/// rejected. Explicit recipes remain useful for caller-owned new dependencies.
llvm::Expected<std::vector<MachOAuthenticatedFixup>>
collectMachOAuthenticatedFixups(llvm::ArrayRef<uint8_t> Original,
                                const CompiledImage &Image);

/// Install new recipes in writable file-backed storage. Imports name an exact
/// existing dylib ordinal (or a dyld special ordinal); Target is already the
/// complete local target including its addend. Existing fixups are preserved.
/// The image is changed only after strict decoding and recipe replay succeed.
/// This writes dyld recipes, never process-signed pointer values. The caller
/// still owns final code signing and the call/unwind ABI publication contract.
llvm::Error
installMachOAuthenticatedFixups(std::vector<uint8_t> &Binary,
                                llvm::ArrayRef<MachOAuthenticatedFixup> Fixups);

/// Replay installed recipes after subsequent image edits, retaining exact
/// storage addresses, import identities and signing discriminators.
llvm::Error
verifyMachOAuthenticatedFixups(llvm::ArrayRef<uint8_t> Binary,
                               llvm::ArrayRef<MachOAuthenticatedFixup> Fixups);

} // namespace neverd

#endif // NEVERD_BACKEND_CODEGEN_MACHO_MACHOAUTHENTICATEDFIXUPS_H
