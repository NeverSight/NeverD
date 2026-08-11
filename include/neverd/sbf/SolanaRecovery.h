//===- SolanaRecovery.h - Solana program fact recovery --------*- C++ -*-===//
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

#ifndef NEVERD_SBF_SOLANARECOVERY_H
#define NEVERD_SBF_SOLANARECOVERY_H

#include "neverd/sbf/Anchor.h"
#include "neverd/sbf/SolanaModel.h"

#include <string>

namespace neverd::sbf {

struct SBFProgram;

struct SolanaRecoveryOptions {
  /// An IDL supplied by the operator. When present its names take precedence
  /// over the built-in dictionary.
  const AnchorIdl *Idl = nullptr;
};

SolanaModel recoverSolanaModel(const SBFProgram &Program,
                               const SolanaRecoveryOptions &Options = {});

std::string dumpSolanaModel(const SolanaModel &Model);

} // namespace neverd::sbf

#endif // NEVERD_SBF_SOLANARECOVERY_H
