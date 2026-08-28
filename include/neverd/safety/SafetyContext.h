//===- SafetyContext.h - Inputs shared by the safety analyses ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The bundle of already-produced analysis artifacts the audit and hunt tracks
/// read: the loaded image, the lifted MedIR and LowIR, and the identity view
/// (debug context, its origin, and caller renames).  The owning PipelineResult
/// binds coverage validation to the exact IR inventories without coupling the
/// engine to a session, so the same analysis runs on any native format the
/// loaders already understand.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SAFETY_SAFETYCONTEXT_H
#define NEVERD_SAFETY_SAFETYCONTEXT_H

#include "neverd/Common.h"
#include "neverd/debug/DebugInfoDiscovery.h"

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace neverd {

struct BinaryImage;
struct MedFunc;
struct LowFunc;
struct PipelineResult;
class DebugContext;

namespace safety {

/// A read-only view of everything the analyses consume.  The pipeline must have
/// run in lift mode so each MedFunc carries recovered call arguments.
struct AnalysisInput {
  const BinaryImage *Img = nullptr;
  const std::vector<MedFunc> *MedFuncs = nullptr;
  const std::vector<LowFunc> *LowFuncs = nullptr;
  /// The PipelineResult that owns MedFuncs and LowFuncs. Public safety entry
  /// points revalidate this exact object; a detached inventory cannot claim
  /// decode, lift, verifier, and call-inventory completeness.
  const PipelineResult *ValidatedPipeline = nullptr;

  const DebugContext *Dbg = nullptr;
  DebugInfoKind DebugKind = DebugInfoKind::None;

  /// Register identities for the loaded architecture, supplied by the caller so
  /// the analysis need not depend on the lifter.  When \c StackRegsKnown is
  /// false the stack-frame reasoning is skipped and capacity falls to other
  /// sources.
  uint64_t StackPointerReg = 0;
  uint64_t FramePointerReg = 0;
  bool StackRegsKnown = false;

  /// Caller renames, keyed by function entry VA — the strongest identity.
  const std::map<va_t, std::string> *Renames = nullptr;

  /// Names established by signature matching, keyed by entry VA.  Keeping the
  /// name and its origin together prevents a signature-only identity from being
  /// labelled `sig` while the scanner still tries to match a stale placeholder.
  const std::unordered_map<va_t, std::string> *SignatureNames = nullptr;

  const MedFunc *findMedFunc(va_t Entry) const;
  const LowFunc *findLowFunc(va_t Entry) const;
};

} // namespace safety
} // namespace neverd

#endif // NEVERD_SAFETY_SAFETYCONTEXT_H
