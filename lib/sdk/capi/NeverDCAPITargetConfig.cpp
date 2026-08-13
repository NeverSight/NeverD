//===- NeverDCAPITargetConfig.cpp - C API: VM target configuration --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// EVM and Solana SBF analysis target configuration.
///
//===----------------------------------------------------------------------===//

#include "SessionImpl.h"

#include "neverd/evm/analysis/EVMAnalyzer.h"
#include "neverd/sbf/analysis/SBFAnalyzer.h"

using namespace neverd;
using namespace neverd::sdk;

void neverd_evm_set_strict(neverd_session_t Sess, int Strict) {
  auto *S = static_cast<Session *>(Sess);
  if (!S)
    return;
  S->clearError();
  S->EVMStrict = Strict != 0;
  S->invalidatePipeline();
}

int neverd_evm_set_hardfork(neverd_session_t Sess, const char *Hardfork) {
  auto *S = static_cast<Session *>(Sess);
  if (!S || !Hardfork)
    return 0;
  auto Fork = evm::parseHardfork(Hardfork);
  if (!Fork) {
    S->setError("unknown EVM hardfork: " + std::string(Hardfork));
    return 0;
  }
  S->clearError();
  S->EVMFork = *Fork;
  S->invalidatePipeline();
  return 1;
}

void neverd_sbf_set_strict(neverd_session_t Sess, int Strict) {
  auto *S = static_cast<Session *>(Sess);
  if (!S)
    return;
  S->clearError();
  S->SBFStrict = Strict != 0;
  S->invalidatePipeline();
}

int neverd_sbf_set_version(neverd_session_t Sess, const char *Version) {
  auto *S = static_cast<Session *>(Sess);
  if (!S || !Version)
    return 0;
  auto Parsed = sbf::parseVersion(Version);
  if (!Parsed) {
    S->setError("unknown SBF version: " + std::string(Version));
    return 0;
  }
  S->clearError();
  S->SBFVersion = *Parsed;
  S->invalidatePipeline();
  return 1;
}

int neverd_sbf_set_cluster(neverd_session_t Sess, const char *Cluster) {
  auto *S = static_cast<Session *>(Sess);
  if (!S || !Cluster)
    return 0;
  auto Parsed = sbf::parseCluster(Cluster);
  if (!Parsed) {
    S->setError("unknown Solana cluster: " + std::string(Cluster));
    return 0;
  }
  S->clearError();
  S->SBFProfile.OnCluster = *Parsed;
  S->invalidatePipeline();
  return 1;
}

void neverd_sbf_set_slot(neverd_session_t Sess, neverd_slot_t Slot) {
  auto *S = static_cast<Session *>(Sess);
  if (!S)
    return;
  S->clearError();
  S->SBFProfile.Slot = Slot;
  S->invalidatePipeline();
}

int neverd_sbf_set_loader(neverd_session_t Sess, const char *Loader) {
  auto *S = static_cast<Session *>(Sess);
  if (!S || !Loader)
    return 0;
  auto Parsed = sbf::parseLoader(Loader);
  if (!Parsed) {
    S->setError("unknown Solana loader: " + std::string(Loader));
    return 0;
  }
  S->clearError();
  S->SBFProfile.OwningLoader = *Parsed;
  S->invalidatePipeline();
  return 1;
}

int neverd_sbf_set_purpose(neverd_session_t Sess, const char *Purpose) {
  auto *S = static_cast<Session *>(Sess);
  if (!S || !Purpose)
    return 0;
  auto Parsed = sbf::parseRuntimePurpose(Purpose);
  if (!Parsed) {
    S->setError("unknown runtime purpose: " + std::string(Purpose));
    return 0;
  }
  S->clearError();
  S->SBFProfile.Purpose = *Parsed;
  S->invalidatePipeline();
  return 1;
}

int neverd_sbf_set_idl(neverd_session_t Sess, const char *Json) {
  auto *S = static_cast<Session *>(Sess);
  if (!S)
    return 0;
  if (!Json || *Json == '\0') {
    S->clearError();
    S->SBFIdl.reset();
    S->invalidatePipeline();
    return 1;
  }
  llvm::Expected<sbf::AnchorIdl> Parsed = sbf::parseAnchorIdl(Json);
  if (!Parsed) {
    S->setError(llvm::toString(Parsed.takeError()));
    return 0;
  }
  S->clearError();
  S->SBFIdl = std::move(*Parsed);
  S->invalidatePipeline();
  return 1;
}
