//===- ExceptionEncoding.h - Native runtime-function encodings -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Names the native representation a normalized exception record was decoded
/// from, and maps each one onto the dispatch model it belongs to.  Keeping the
/// model a pure function of the encoding means a consumer cannot see a record
/// whose model and encoding disagree.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_EXCEPTIONENCODING_H
#define NEVERD_LOADER_EXCEPTIONENCODING_H

#include "neverd/loader/ExceptionModel.h"

#include <cstdint>

namespace neverd {

/// Native runtime-function representation that produced a normalized record.
enum class ExceptionEncoding : uint8_t {
  Unknown,
  X64UnwindV1,
  X64UnwindV2,
  X64UnwindV3,
  ARM32Packed,
  ARM32PackedFragment,
  ARM32Unpacked,
  ARM64Packed,
  ARM64PackedFragment,
  ARM64Unpacked,
  /// DWARF call frame information: one FDE and its CIE.
  DwarfFDE,
  /// Darwin `__unwind_info` second-level compact entry.
  CompactUnwind,
  /// x86-32 `_except_handler3` scope table reached through the registration
  /// chain the prologue installed.
  X86ScopeTableEH3,
  /// x86-32 `_except_handler4` scope table, which prefixes the entry array
  /// with security-cookie displacements.
  X86ScopeTableEH4,
  /// x86-32 `__CxxFrameHandler` `FuncInfo`, whose maps hold absolute
  /// pointers rather than the image-relative fields the x64 form uses.
  X86CxxFuncInfo,
  /// Delphi's x86-32 registration frame, whose handler is a runtime routine
  /// rather than a table-driven dispatcher.
  DelphiX86Chain,
  /// Go `pclntab` frame metadata.
  GoFuncTable,
  /// ARM EHABI `EXIDX_CANTUNWIND`: the index covers the frame in order to say
  /// that it may not be unwound through.
  ARMEHABICantUnwind,
  /// ARM EHABI index entry whose own word holds the whole descriptor.
  ARMEHABIInline,
  /// ARM EHABI `.ARM.extab` entry using an ARM-defined personality index.
  ARMEHABICompact,
  /// ARM EHABI `.ARM.extab` entry naming its personality routine, which is
  /// the form that carries an Itanium LSDA inline after its unwind opcodes.
  ARMEHABIGeneric,
};

inline const char *getExceptionEncodingName(ExceptionEncoding Encoding) {
  switch (Encoding) {
  case ExceptionEncoding::X64UnwindV1:
    return "x64-unwind-v1";
  case ExceptionEncoding::X64UnwindV2:
    return "x64-unwind-v2";
  case ExceptionEncoding::X64UnwindV3:
    return "x64-unwind-v3";
  case ExceptionEncoding::ARM32Packed:
    return "arm32-packed";
  case ExceptionEncoding::ARM32PackedFragment:
    return "arm32-packed-fragment";
  case ExceptionEncoding::ARM32Unpacked:
    return "arm32-unpacked";
  case ExceptionEncoding::ARM64Packed:
    return "arm64-packed";
  case ExceptionEncoding::ARM64PackedFragment:
    return "arm64-packed-fragment";
  case ExceptionEncoding::ARM64Unpacked:
    return "arm64-unpacked";
  case ExceptionEncoding::DwarfFDE:
    return "dwarf-fde";
  case ExceptionEncoding::CompactUnwind:
    return "compact-unwind";
  case ExceptionEncoding::X86ScopeTableEH3:
    return "x86-scope-table-eh3";
  case ExceptionEncoding::X86ScopeTableEH4:
    return "x86-scope-table-eh4";
  case ExceptionEncoding::X86CxxFuncInfo:
    return "x86-cxx-funcinfo";
  case ExceptionEncoding::DelphiX86Chain:
    return "delphi-x86-chain";
  case ExceptionEncoding::GoFuncTable:
    return "go-func-table";
  case ExceptionEncoding::ARMEHABICantUnwind:
    return "arm-ehabi-cantunwind";
  case ExceptionEncoding::ARMEHABIInline:
    return "arm-ehabi-inline";
  case ExceptionEncoding::ARMEHABICompact:
    return "arm-ehabi-compact";
  case ExceptionEncoding::ARMEHABIGeneric:
    return "arm-ehabi-generic";
  case ExceptionEncoding::Unknown:
    return "unknown";
  }
  return "unknown";
}

/// The exception model a native encoding belongs to.  Keeping this a pure
/// function of the encoding means a consumer cannot see a record whose model
/// and encoding disagree.
inline ExceptionModel getExceptionEncodingModel(ExceptionEncoding Encoding) {
  switch (Encoding) {
  case ExceptionEncoding::X64UnwindV1:
  case ExceptionEncoding::X64UnwindV2:
  case ExceptionEncoding::X64UnwindV3:
  case ExceptionEncoding::ARM32Packed:
  case ExceptionEncoding::ARM32PackedFragment:
  case ExceptionEncoding::ARM32Unpacked:
  case ExceptionEncoding::ARM64Packed:
  case ExceptionEncoding::ARM64PackedFragment:
  case ExceptionEncoding::ARM64Unpacked:
    return ExceptionModel::WindowsTable;
  case ExceptionEncoding::DwarfFDE:
    return ExceptionModel::Itanium;
  case ExceptionEncoding::CompactUnwind:
    return ExceptionModel::CompactUnwind;
  case ExceptionEncoding::X86ScopeTableEH3:
  case ExceptionEncoding::X86ScopeTableEH4:
  case ExceptionEncoding::X86CxxFuncInfo:
  case ExceptionEncoding::DelphiX86Chain:
    return ExceptionModel::WindowsRegistration;
  case ExceptionEncoding::GoFuncTable:
    return ExceptionModel::GoRuntime;
  case ExceptionEncoding::ARMEHABICantUnwind:
  case ExceptionEncoding::ARMEHABIInline:
  case ExceptionEncoding::ARMEHABICompact:
  case ExceptionEncoding::ARMEHABIGeneric:
    return ExceptionModel::ARMEHABI;
  case ExceptionEncoding::Unknown:
    return ExceptionModel::None;
  }
  return ExceptionModel::None;
}

enum class RuntimeFunctionKind : uint8_t {
  Primary,
  Chained,
  Fragment,
};

} // namespace neverd

#endif // NEVERD_LOADER_EXCEPTIONENCODING_H
