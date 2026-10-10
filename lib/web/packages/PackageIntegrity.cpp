//===- PackageIntegrity.cpp - Original archive SRI verification ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared bounded declaration parsing and strongest-algorithm byte comparison.
///
//===----------------------------------------------------------------------===//

#include "neverd/web/PackageIntegrity.h"

#include "../BlobStore.h"
#include "../JsonReader.h"
#include "SHA512.h"

#include "neverd/web/Error.h"
#include "neverd/web/Limits.h"

#include "llvm/Support/Base64.h"
#include "llvm/Support/SHA1.h"
#include "llvm/Support/SHA256.h"

namespace neverd::web {
namespace {
struct Declaration {
  std::string Status = "invalid", Algorithm;
  unsigned Strength = 0;
  std::vector<std::string> Digests;
};
Declaration parse(std::string_view Value, bool Git) {
  Declaration D;
  if (Git && Value.size() == 40 &&
      Value.find_first_not_of("0123456789abcdefABCDEF") ==
          std::string_view::npos) {
    D.Status = "git_commit_declared_unverified";
    return D;
  }
  if (Value.size() > 65536) {
    D.Status = "integrity_declaration_budget_exceeded";
    return D;
  }
  bool Unsupported = false;
  unsigned Tokens = 0;
  while (!Value.empty()) {
    const auto Start = Value.find_first_not_of(" \t\r\n");
    if (Start == std::string_view::npos)
      break;
    Value.remove_prefix(Start);
    if (++Tokens > 256) {
      D.Status = "integrity_declaration_budget_exceeded";
      return D;
    }
    const auto End = Value.find_first_of(" \t\r\n");
    const auto Token = Value.substr(0, End);
    const auto Dash = Token.find('-');
    if (!Dash || Dash == std::string_view::npos)
      return D;
    const auto Algorithm = Token.substr(0, Dash);
    constexpr std::string_view Letters =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    if (Algorithm.find_first_not_of(Letters) != std::string_view::npos)
      return D;
    const auto Encoded = Token.substr(Dash + 1);
    if (Encoded.find('?') != std::string_view::npos) {
      D.Status = "unsupported_sri_options";
      return D;
    }
    const unsigned Length = Algorithm == "sha512"   ? 64
                            : Algorithm == "sha384" ? 48
                            : Algorithm == "sha256" ? 32
                            : Algorithm == "sha1"   ? 20
                                                    : 0;
    constexpr std::string_view Alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (Encoded.empty() || Encoded.size() % 4)
      return D;
    const auto Padding = Encoded.ends_with("==")  ? 2
                         : Encoded.ends_with('=') ? 1
                                                  : 0;
    const auto Data = Encoded.substr(0, Encoded.size() - Padding);
    if (Data.empty() ||
        Data.find_first_not_of(Alphabet) != std::string_view::npos ||
        (Padding && (Alphabet.find(Data.back()) & (Padding == 2 ? 15 : 3))) ||
        (Length && Encoded.size() / 4 * 3 - Padding != Length))
      return D;
    if (Length > D.Strength) {
      D.Strength = Length;
      D.Algorithm = Algorithm;
      D.Digests.clear();
    }
    if (Length && Length == D.Strength)
      D.Digests.emplace_back(Encoded);
    Unsupported |= !Length;
    if (End == std::string_view::npos)
      break;
    Value.remove_prefix(End);
  }
  D.Status = D.Strength    ? "declared_unverified"
             : Unsupported ? "unsupported_algorithm"
                           : "invalid";
  return D;
}
std::string digest(const Blob &Bytes, std::string_view Algorithm) {
  llvm::SHA1 S1;
  llvm::SHA256 S256;
  SHA512 S512(Algorithm == "sha384");
  for (uint64_t Offset = 0; Offset < Bytes.size();) {
    const auto N = std::min(BlobTransferBytes, Bytes.size() - Offset);
    const auto Data = Bytes.read(Offset, N);
    if (Algorithm == "sha1")
      S1.update(Data);
    else if (Algorithm == "sha256")
      S256.update(Data);
    else
      S512.update(Data);
    Offset += N;
  }
  if (Algorithm == "sha1")
    return llvm::encodeBase64(S1.final());
  if (Algorithm == "sha256")
    return llvm::encodeBase64(S256.final());
  return llvm::encodeBase64(S512.result());
}
} // namespace

std::string packageIntegrityDeclarationStatus(std::string_view Value,
                                              bool Git) {
  return parse(Value, Git).Status;
}

PackageIntegrityDeclaration registryPackageIntegrity(const Artifact &Metadata) {
  if (Metadata.Directory || Metadata.Content.size() > MaxPackageMetadataBytes)
    throw Error("integrity_metadata_budget_exceeded");
  const auto Bytes = Metadata.Content.read(0, Metadata.Content.size(),
                                           MaxPackageMetadataBytes);
  if (sha256(Bytes) != Metadata.BlobHash)
    throw Error("integrity_metadata_hash_mismatch");
  auto V =
      parseBoundedJSON(Bytes, {MaxPackageMetadataBytes, 64, 200000, 65536});
  const auto *O = V.getAsObject();
  if (!O)
    throw Error("integrity_metadata_not_object");
  const auto *Dist = O->getObject("dist");
  if (!Dist)
    throw Error("integrity_registry_dist_missing");
  PackageIntegrityDeclaration D{
      Metadata.ID, Metadata.BlobHash, Metadata.ID, "registry_dist", {}, false};
  if (const auto *I = Dist->get("integrity")) {
    if (!I->getAsString())
      throw Error("integrity_metadata_invalid_type");
    D.Value = I->getAsString()->str();
  }
  return D;
}

PackageIntegrityDeclaration lockedPackageIntegrity(const PackageAnalysis &A,
                                                   std::string_view PackageID) {
  if (A.Kind != "npm-lock")
    throw Error("integrity_declaration_not_lock");
  for (const auto &P : A.Packages)
    if (P.ID == PackageID)
      return {A.ArtifactID,
              A.BlobHash,
              P.ID,
              "lock_package",
              !P.Integrity && P.IntegrityStatus == "invalid"
                  ? std::optional<std::string>("")
                  : P.Integrity,
              P.IntegrityStatus == "git_commit_declared_unverified"};
  throw Error("unknown_package_instance");
}

PackageIntegrityResult
verifyPackageIntegrity(const Artifact &Original,
                       const PackageIntegrityDeclaration &D) {
  if (Original.Directory || Original.Content.size() > Limits::HardInputBytes)
    throw Error("integrity_invalid_original");
  if (Original.Content.digest() != Original.BlobHash)
    throw Error("integrity_original_hash_mismatch");
  PackageIntegrityResult R;
  R.ArtifactID = Original.ID;
  R.BlobHash = Original.BlobHash;
  R.DeclarationArtifactID = D.ArtifactID;
  R.DeclarationHash = D.BlobHash;
  R.SelectionID = D.SelectionID;
  R.Kind = D.Kind;
  R.Bytes = Original.Content.size();
  R.ID = identity("package-integrity",
                  {R.ArtifactID, R.BlobHash, D.ArtifactID, D.BlobHash,
                   D.SelectionID, D.Kind, PackageIntegrityProfile});
  R.Status = "missing_declaration";
  if (!D.Value)
    return R;
  const auto Parsed = parse(*D.Value, D.Git);
  R.Status = Parsed.Status;
  if (R.Status != "declared_unverified")
    return R;
  R.Algorithm = Parsed.Algorithm;
  R.CandidateCount = Parsed.Digests.size();
  const auto Actual = digest(Original.Content, R.Algorithm);
  R.Status = std::find(Parsed.Digests.begin(), Parsed.Digests.end(), Actual) !=
                     Parsed.Digests.end()
                 ? "match"
                 : "mismatch";
  return R;
}
} // namespace neverd::web
