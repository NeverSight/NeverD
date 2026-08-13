//===- SBFSolanaRecoveryPrinter.cpp - Textual dump of the model -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Renders the recovered Solana model as text: the program's own address, the
/// addresses and discriminators found in read-only data, the invocations and
/// derivations, the returned errors, the input accesses, and the lints.
///
//===----------------------------------------------------------------------===//

#include "neverd/sbf/runtime/SBFSyscalls.h"
#include "neverd/sbf/solana/SBFAccountLayout.h"
#include "neverd/sbf/solana/SBFCPI.h"
#include "neverd/sbf/solana/SBFKnownAddresses.h"
#include "neverd/sbf/solana/SBFPubkey.h"
#include "neverd/sbf/solana/SBFSolanaRecovery.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <string>

namespace neverd::sbf {

std::string dumpSolanaModel(const SolanaModel &Model) {
  std::string Buffer;
  llvm::raw_string_ostream OS(Buffer);

  if (Model.ProgramId)
    OS << "program-id " << formatPubkey(*Model.ProgramId) << "\n";
  if (Model.IsAnchor)
    OS << "framework anchor" << (Model.IdlName.empty() ? "" : " idl=")
       << Model.IdlName << "\n";

  for (const RecoveredPubkey &Key : Model.Pubkeys) {
    OS << "pubkey 0x" << llvm::utohexstr(Key.Address) << " "
       << formatPubkey(Key.Key);
    if (Key.Known)
      OS << " " << Key.Known->Name << " ("
         << knownAddressCategoryName(Key.Known->Category) << ")";
    if (Key.ReferencedByCode)
      OS << " referenced";
    OS << "\n";
  }

  for (const AnchorHandler &Handler : Model.Handlers) {
    OS << "handler slot " << Handler.CompareSlot << " 0x"
       << llvm::toHex(Handler.Discriminator.Bytes, /*LowerCase=*/true);
    if (!Handler.Name.empty())
      OS << " " << anchorNamespaceSpelling(Handler.Namespace) << " "
         << Handler.Name << " (" << recoveryEvidenceName(*Handler.NameEvidence)
         << ")";
    if (Handler.TargetSlot)
      OS << " -> slot " << *Handler.TargetSlot;
    OS << "\n";
  }

  for (const RecoveredDiscriminator &Found : Model.Discriminators)
    OS << "discriminator 0x" << llvm::utohexstr(Found.Address) << " "
       << anchorNamespaceSpelling(Found.Namespace) << " " << Found.Name << " ("
       << recoveryEvidenceName(Found.Evidence) << ")\n";

  for (const CPISite &Site : Model.CPISites) {
    OS << "cpi slot " << Site.Slot;
    if (const SyscallInfo *Info = getSyscallInfo(Site.Which))
      OS << " " << Info->Name;
    if (Site.ProgramId)
      OS << " -> " << formatPubkey(*Site.ProgramId);
    if (Site.KnownProgram)
      OS << " " << Site.KnownProgram->Name;
    if (Site.Selected)
      OS << "::" << Site.Selected->Name;
    else if (!Site.Name.empty())
      OS << "::" << Site.Name << " ("
         << recoveryEvidenceName(*Site.NameEvidence) << ")";
    else if (Site.Discriminator)
      OS << "::0x"
         << llvm::toHex(Site.Discriminator->Bytes, /*LowerCase=*/true);
    if (Site.AccountCount)
      OS << " accounts=" << *Site.AccountCount;
    if (Site.DataLength)
      OS << " data=" << *Site.DataLength;
    OS << "\n";
  }

  for (const PDADerivation &Derivation : Model.Derivations) {
    OS << "pda slot " << Derivation.Slot;
    if (const SyscallInfo *Info = getSyscallInfo(Derivation.Which))
      OS << " " << Info->Name;
    if (Derivation.DeclaredSeedCount)
      OS << " seeds=" << Derivation.Seeds.size() << "/"
         << *Derivation.DeclaredSeedCount;
    if (Derivation.KnownProgram)
      OS << " " << Derivation.KnownProgram->Name;
    else if (Derivation.ProgramId)
      OS << " " << formatPubkey(*Derivation.ProgramId);
    OS << "\n";
    for (const RecoveredSeed &Seed : Derivation.Seeds) {
      OS << "  seed";
      if (Seed.Address)
        OS << " 0x" << llvm::utohexstr(*Seed.Address);
      if (Seed.Length)
        OS << " len " << *Seed.Length;
      if (Seed.isText())
        OS << " \""
           << llvm::StringRef(reinterpret_cast<const char *>(Seed.Bytes.data()),
                              Seed.Bytes.size())
           << "\"";
      else if (!Seed.Bytes.empty())
        OS << " 0x" << llvm::toHex(Seed.Bytes, /*LowerCase=*/true);
      OS << "\n";
    }
  }

  for (const ReturnedError &Error : Model.Errors) {
    OS << "error " << Error.Code;
    if (const AnchorErrorInfo *Known = Error.Classification.Known)
      OS << " " << Known->Name << ": " << Known->Message;
    else if (Error.Classification.CustomOrdinal)
      OS << " " << Error.Classification.Range->Name << " #"
         << *Error.Classification.CustomOrdinal;
    else
      OS << " " << Error.Classification.Range->Name;
    OS << "\n";
  }

  for (const AccountAccess &Access : Model.AccountAccesses) {
    OS << (Access.IsWrite ? "input-store slot " : "input-load slot ")
       << Access.Slot << " +" << Access.InputOffset;
    if (Access.Header)
      OS << " " << getInputFieldInfo(*Access.Header).Name;
    if (Access.Field)
      OS << " accounts[0]." << getAccountFieldName(*Access.Field).Name;
    OS << "\n";
  }

  for (const LintFinding &Finding : Model.Findings) {
    const LintInfo &Info = getLintInfo(Finding.ID);
    OS << lintSeverityName(Info.Severity) << " " << Info.Name << " ["
       << lintConfidenceName(Info.Confidence) << "]";
    if (Finding.Slot)
      OS << " slot " << *Finding.Slot;
    if (!Finding.Detail.empty())
      OS << ": " << Finding.Detail;
    OS << "\n";
  }

  return Buffer;
}

} // namespace neverd::sbf
