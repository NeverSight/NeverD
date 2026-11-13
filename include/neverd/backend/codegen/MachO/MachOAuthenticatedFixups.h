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
struct SourcePatchDependency;

struct MachOAuthenticatedFixup {
  va_t Address = 0;
  MachOAuthenticatedPointer Recipe;
  /// Require dyld's post-fixup read-only protection on the owning segment.
  bool ReadOnlyAfterFixups = false;
  /// False for an ordinary pointer sharing the same dyld chain. Signing
  /// fields must then be zero; Target/import identity retain their meaning.
  bool Authenticated = true;
  /// Convert an existing IA/0 libSystem TLV bootstrap bind to the local
  /// per-thread resolver. All other existing slots retain their identity.
  bool ReplacesTLVBootstrap = false;
};

/// Translate producer receipts at their final section VAs. External symbols
/// identify one exact dyld provider. Explicit declarations select a library
/// already admitted to Original; otherwise conflicting providers are rejected.
/// Every declaration must be used by an undefined loader pointer reference.
llvm::Expected<std::vector<MachOAuthenticatedFixup>>
collectMachOAuthenticatedFixups(
    llvm::ArrayRef<uint8_t> Original, const CompiledImage &Image,
    llvm::ArrayRef<SourcePatchDependency> Dependencies = {});

/// Install new recipes in writable file-backed storage. Imports name an exact
/// existing dylib ordinal (or a dyld special ordinal); Target is already the
/// complete local target including its addend. Existing fixups are preserved.
/// The image is changed only after strict decoding and recipe replay succeed.
/// This writes dyld recipes, never process-signed pointer values. The caller
/// still owns final code signing and the call/unwind ABI publication contract.
llvm::Error
installMachOAuthenticatedFixups(std::vector<uint8_t> &Binary,
                                llvm::ArrayRef<MachOAuthenticatedFixup> Fixups);

/// Explicitly migrate a legacy threaded image to LC_DYLD_CHAINED_FIXUPS,
/// retaining exact slot, library, symbol and export identities. Already
/// chained images are unchanged. Unsupported import flags reject without
/// mutation. The caller must replace the code signature after conversion.
llvm::Error convertMachOThreadedFixupsToChains(std::vector<uint8_t> &Binary);

/// Prepare userland fixups for default dyld startup. B-key chains use 4 KiB
/// pages so dyld can apply the full signing recipe when the native pager
/// cannot. Legacy threaded metadata is migrated to modern chained metadata.
/// Slot identities, protections and code are preserved; the caller owns
/// signing.
llvm::Error prepareMachOAuthenticatedStartup(std::vector<uint8_t> &Binary);

/// Replay installed recipes after subsequent image edits, retaining exact
/// storage addresses, import identities, signing discriminators and requested
/// post-fixup read-only protection.
llvm::Error
verifyMachOAuthenticatedFixups(llvm::ArrayRef<uint8_t> Binary,
                               llvm::ArrayRef<MachOAuthenticatedFixup> Fixups);

} // namespace neverd

#endif // NEVERD_BACKEND_CODEGEN_MACHO_MACHOAUTHENTICATEDFIXUPS_H
