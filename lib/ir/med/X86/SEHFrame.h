//===- SEHFrame.h - Windows frames in converted CFGs -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Share the converted frame check and its current block/call sources.

#ifndef NEVERD_IR_MED_X86_SEHFRAME_H
#define NEVERD_IR_MED_X86_SEHFRAME_H

#include "neverd/loader/ExceptionCommon.h"

#include "llvm/ADT/StringRef.h"

#include <map>
#include <optional>
#include <set>
#include <vector>

namespace neverd {
struct LowFunc;
struct LowSEHEstablisherFrame;
struct MedFunc;
struct TargetRegInfo;

namespace x86_seh {
void projectProtectedSources(const LowFunc &Low, const MedFunc &Med,
                             const LowSEHEstablisherFrame &Proof,
                             size_t &Remaining, std::vector<int> *Sources,
                             std::map<int, std::set<va_t>> *Calls);
std::optional<uint64_t> proveSEHEstablisherFrame(
    const LowFunc &Low, const MedFunc &Med, va_t Handler,
    const TargetRegInfo &TRI, bool OrdinaryEntry,
    llvm::StringRef LocalUnwindDigest = {}, bool CxxContinuation = false,
    std::vector<int> *ProtectedSources = nullptr,
    std::map<int, std::set<va_t>> *ProtectedCalls = nullptr,
    bool SourceProjection = false, bool *UnresolvedEntries = nullptr);
} // namespace x86_seh
} // namespace neverd

#endif
