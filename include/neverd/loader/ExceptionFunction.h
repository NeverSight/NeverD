//===- ExceptionFunction.h - Per-function exception record ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// One normalized runtime-function record: the code it covers, the native
/// table it came from, the unwind actions it declares, and whichever of the
/// language models decoded alongside it.  A record carries several language
/// readings at once where the models genuinely overlap, because a Rust or
/// Objective-C frame annotates a table it shares rather than replacing it.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_EXCEPTIONFUNCTION_H
#define NEVERD_LOADER_EXCEPTIONFUNCTION_H

#include "neverd/loader/ExceptionCommon.h"
#include "neverd/loader/ExceptionEncoding.h"
#include "neverd/loader/ExceptionModel.h"
#include "neverd/loader/ExceptionPersonality.h"
#include "neverd/loader/ExceptionUnwindOp.h"
#include "neverd/loader/ExceptionWindowsEH.h"
#include "neverd/loader/LanguageEHARM.h"
#include "neverd/loader/LanguageEHCompact.h"
#include "neverd/loader/LanguageEHDelphi.h"
#include "neverd/loader/LanguageEHDwarf.h"
#include "neverd/loader/LanguageEHGo.h"
#include "neverd/loader/LanguageEHItanium.h"
#include "neverd/loader/LanguageEHObjC.h"
#include "neverd/loader/LanguageEHRegistration.h"
#include "neverd/loader/LanguageEHRust.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neverd {

struct ExceptionFunction {
  ExceptionAddressRange CodeRange;
  RuntimeFunctionKind Kind = RuntimeFunctionKind::Primary;
  ExceptionEncoding Encoding = ExceptionEncoding::Unknown;
  ExceptionParseStatus ParseStatus = ExceptionParseStatus::Complete;

  /// Native table provenance.  RVAs are retained for diagnostics and patch
  /// replacement; addresses are normalized image VAs for IR consumers.
  uint32_t RuntimeFunctionRVA = 0;
  uint32_t UnwindInfoRVA = 0;
  va_t UnwindInfoVA = 0;
  uint8_t UnwindVersion = 0;
  uint8_t UnwindFlags = 0;
  uint32_t PrologueSize = 0;
  uint16_t FrameRegister = 0;
  uint32_t FrameOffset = 0;
  uint32_t PackedUnwindData = 0;
  std::vector<uint8_t> NativeUnwindBytes;
  std::vector<UnwindOperation> UnwindOperations;
  std::vector<UnwindEpilog> Epilogs;

  va_t PersonalityVA = 0;
  va_t HandlerDataVA = 0;
  ExceptionPersonality Personality = ExceptionPersonality::None;
  std::string PersonalityName;
  std::optional<SEHExceptionInfo> SEH;
  std::optional<CxxExceptionInfo> Cxx;
  std::optional<GSCookieInfo> GSCookie;

  /// Itanium model: the DWARF frame description and its LSDA.
  std::optional<DwarfFDE> Dwarf;
  std::optional<ItaniumEHInfo> Itanium;
  /// ARM EHABI model: the index entry and the `.ARM.extab` entry it reached.
  /// Present beside \ref Itanium rather than instead of it, because a C++
  /// frame on this target keeps its language data inside the EHABI entry --
  /// the two describe one frame and neither is readable without the other.
  std::optional<ARMEHABIInfo> ARMEHABI;
  /// Darwin compact-unwind entry covering this range.
  std::optional<CompactUnwindEntry> Compact;
  /// x86-32 registration model: the chain the prologue installed.
  std::optional<RegistrationChainInfo> Registration;
  /// Delphi x86-32 model: the `TExcFrame` the prologue linked, present instead
  /// of \ref Registration because such a frame has no scope table to fill it.
  std::optional<DelphiFrameInfo> Delphi;
  /// Delphi x86-64 model: the `TExcData` scope array in the handler data.
  /// Delphi dropped the registration chain on this target, so a frame carries
  /// one of these or a \ref Delphi record but never both.
  std::optional<DelphiScopeTable> DelphiScopes;
  /// Go model: the runtime's frame metadata for this function.
  std::optional<GoFunctionEH> Go;
  /// Rust reading of whichever table model this record already carries.  Rust
  /// shares the Itanium LSDA with C++ and the MSVC `FuncInfo` with Windows
  /// C++, so this does not replace either -- it says what the shared structure
  /// means for a Rust frame.
  std::optional<RustFunctionEH> Rust;
  /// Objective-C reading of whichever table model this record already carries.
  /// Like \ref Rust this annotates a shared structure rather than replacing
  /// it: Objective-C borrows the Itanium LSDA and, on `*-windows-msvc`, the
  /// MSVC `FuncInfo`, and what differs is how the type table is read and what
  /// a pad's runtime calls say it is for.
  std::optional<ObjCFunctionEH> ObjC;

  /// Index of the primary record for a chained/fragment record, when known.
  std::optional<size_t> PrimaryFunctionIndex;
  std::optional<ExceptionAddressRange> ChainedPrimaryRange;
  uint32_t ChainedUnwindInfoRVA = 0;
  std::vector<std::string> Diagnostics;

  /// Replaceable structural and language contributions to the compatibility
  /// parse summary below.  Appended after the original data fields so existing
  /// positional aggregate initializers retain their field mapping.  Records
  /// produced by older decoders may leave this empty; their existing status
  /// and diagnostics then remain authoritative.
  std::optional<ExceptionFunctionDecodeProvenance> DecodeProvenance;

  ExceptionModel model() const { return getExceptionEncodingModel(Encoding); }

  /// Start tracking independently replaceable parse contributions.
  ///
  /// Existing compatibility state becomes the structural baseline so opting
  /// an older record into provenance cannot silently improve or erase it.
  ExceptionFunctionDecodeProvenance &establishDecodeProvenance() {
    if (!DecodeProvenance) {
      DecodeProvenance.emplace();
      DecodeProvenance->Structural.ParseStatus = ParseStatus;
      DecodeProvenance->Structural.Diagnostics = Diagnostics;
    }
    return *DecodeProvenance;
  }

  /// Rebuild the legacy summary fields in a stable, source-defined order.
  void rebuildParseSummary() {
    if (!DecodeProvenance)
      return;
    ParseStatus =
        mergeExceptionParseStatus(DecodeProvenance->Structural.ParseStatus,
                                  DecodeProvenance->Language.ParseStatus);
    Diagnostics = DecodeProvenance->Structural.Diagnostics;
    Diagnostics.insert(Diagnostics.end(),
                       DecodeProvenance->Language.Diagnostics.begin(),
                       DecodeProvenance->Language.Diagnostics.end());
  }

  /// True when this record carries a decoded language table of any model.
  bool hasLanguageTable() const {
    return SEH.has_value() || Cxx.has_value() || Itanium.has_value() ||
           Registration.has_value() || Delphi.has_value() ||
           DelphiScopes.has_value() || Go.has_value();
  }

  bool canRegenerateLanguageMetadata() const {
    // Native regeneration is currently a Windows-table capability.  Every
    // other model is decoded and structured but must not authorize a rewrite
    // that would have to reproduce a contract NeverD cannot yet emit.
    if (model() != ExceptionModel::WindowsTable)
      return false;
    return ParseStatus == ExceptionParseStatus::Complete &&
           !isGSWrappedPersonality(Personality) &&
           Personality != ExceptionPersonality::CxxFrameHandler4 &&
           Encoding != ExceptionEncoding::X64UnwindV3;
  }
};

} // namespace neverd

#endif // NEVERD_LOADER_EXCEPTIONFUNCTION_H
