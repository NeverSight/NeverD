//===- ExceptionTable.h - Image-wide exception table ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The image-wide \ref ExceptionInfo container: every decoded runtime-function
/// record, the state shared between them (CIEs, Go module data, the Rust and
/// Objective-C runtime facts), and the address index that answers which record
/// covers a given VA.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_EXCEPTIONTABLE_H
#define NEVERD_LOADER_EXCEPTIONTABLE_H

#include "neverd/loader/ExceptionCommon.h"
#include "neverd/loader/ExceptionFunction.h"
#include "neverd/loader/ExceptionModel.h"
#include "neverd/loader/LanguageEHDwarf.h"
#include "neverd/loader/LanguageEHGo.h"
#include "neverd/loader/LanguageEHObjC.h"
#include "neverd/loader/LanguageEHRust.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neverd {

/// Image-wide exception table plus a stable address index.  Runtime records
/// may overlap because chained entries and ARM fragments describe one logical
/// function, so lookup returns the most specific containing range.
struct ExceptionInfo {
  std::vector<ExceptionFunction> Functions;
  ExceptionParseStatus ParseStatus = ExceptionParseStatus::Complete;
  uint32_t DirectoryRVA = 0;
  uint32_t DirectorySize = 0;
  std::vector<size_t> FunctionIndex;
  std::vector<std::string> Diagnostics;

  /// Models present in this image.  A single image legitimately carries more
  /// than one: a MinGW PE has both `.pdata` and Itanium tables, and a Go
  /// program that links cgo has Go frames beside DWARF ones.
  std::vector<ExceptionModel> Models;

  /// Which language runtime produced this image, classified from its sections,
  /// symbols, and embedded runtime banners.  A model does not imply a runtime:
  /// an Itanium LSDA is emitted by C, C++, and Rust alike, so a consumer that
  /// needs to know what a landing pad *does* reads this rather than inferring
  /// it from \ref Models.
  LanguageRuntimeInfo Runtime;

  /// Every CIE referenced by a decoded FDE, keyed by its section offset.  CIEs
  /// are shared by many FDEs, so they are stored once here rather than copied
  /// into each function record.
  std::vector<DwarfCIE> CIEs;

  /// Go runtime module state, present when the image carries a `pclntab`.
  /// Held once per image because every Go function record resolves its offsets
  /// against these bases.
  std::optional<GoModuleInfo> GoModule;

  /// Rust panic machinery, present when the image links the Rust runtime.
  std::optional<RustRuntimeInfo> RustRuntime;

  /// Objective-C exception machinery, present when the image links one of the
  /// Objective-C runtimes.
  std::optional<ObjCRuntimeInfo> ObjCRuntime;

  /// Section offset -> index into \ref CIEs.
  const DwarfCIE *findCIE(uint64_t SectionOffset) const {
    for (const DwarfCIE &CIE : CIEs)
      if (CIE.SectionOffset == SectionOffset)
        return &CIE;
    return nullptr;
  }

  bool hasModel(ExceptionModel Model) const {
    return std::find(Models.begin(), Models.end(), Model) != Models.end();
  }

  void addModel(ExceptionModel Model) {
    if (Model != ExceptionModel::None && !hasModel(Model))
      Models.push_back(Model);
  }

  void rebuildIndex() {
    FunctionIndex.resize(Functions.size());
    for (size_t I = 0; I < Functions.size(); ++I)
      FunctionIndex[I] = I;
    std::stable_sort(FunctionIndex.begin(), FunctionIndex.end(),
                     [&](size_t A, size_t B) {
                       const auto &RA = Functions[A].CodeRange;
                       const auto &RB = Functions[B].CodeRange;
                       if (RA.Begin != RB.Begin)
                         return RA.Begin < RB.Begin;
                       return RA.size() < RB.size();
                     });
  }

  const ExceptionFunction *findFunction(va_t Address) const {
    const ExceptionFunction *Best = nullptr;
    for (size_t I : FunctionIndex) {
      if (I >= Functions.size())
        continue;
      const ExceptionFunction &F = Functions[I];
      if (F.CodeRange.Begin > Address)
        break;
      if (!F.CodeRange.contains(Address))
        continue;
      if (!Best || F.CodeRange.size() < Best->CodeRange.size())
        Best = &F;
    }
    return Best;
  }

  ExceptionFunction *findFunction(va_t Address) {
    return const_cast<ExceptionFunction *>(
        static_cast<const ExceptionInfo *>(this)->findFunction(Address));
  }
};

} // namespace neverd

#endif // NEVERD_LOADER_EXCEPTIONTABLE_H
