//===- SBFSolanaRecovery.h - Solana program fact recovery -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Recovers Solana-level facts from an analyzed SBF program. This runs after
/// MedIR and its register dataflow, and reads them without modifying either.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_SOLANA_SBFSOLANARECOVERY_H
#define NEVERD_SBF_SOLANA_SBFSOLANARECOVERY_H

#include "neverd/sbf/runtime/SBFRuntimeProfile.h"
#include "neverd/sbf/solana/SBFAnchor.h"
#include "neverd/sbf/solana/SBFSolanaModel.h"

#include <string>

namespace neverd::sbf {

struct SBFProgram;

struct SolanaRecoveryOptions {
  /// An IDL supplied by the operator. When present its names take precedence
  /// over the built-in dictionary.
  const AnchorIdl *Idl = nullptr;
  /// The runtime the recovered facts are about. It decides which serialization
  /// an input offset is read against and which syscalls a reference resolves
  /// to, neither of which the program file states.
  RuntimeProfile Profile;
};

SolanaModel recoverSolanaModel(const SBFProgram &Program,
                               const SolanaRecoveryOptions &Options = {});

std::string dumpSolanaModel(const SolanaModel &Model);

} // namespace neverd::sbf

#endif // NEVERD_SBF_SOLANA_SBFSOLANARECOVERY_H
