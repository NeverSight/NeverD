//===- LanguageEH.h - Non-Windows exception models ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the normalized records for exception machinery that does not use
/// the Windows table model: the Itanium C++ ABI (DWARF `.eh_frame` call frame
/// information plus a `.gcc_except_table` language-specific data area), the
/// Darwin compact-unwind encoding, the x86-32 registration chain rooted at
/// `FS:[0]`, and the Go runtime's frame-metadata driven defer/panic/recover.
///
/// These records share the checked-range, provenance, and parse-status
/// discipline of \ref ExceptionInfo.h: a decoder never exposes a raw file
/// pointer, never guesses a schema it did not prove, and marks anything it
/// could not fully validate rather than silently producing a plausible record.
///
/// This is an umbrella header.  Each model lives in a header of its own, and
/// this one names them all so that a consumer keeps one include for the whole
/// vocabulary.  Include a part directly when only that model is needed.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_LANGUAGEEH_H
#define NEVERD_LOADER_LANGUAGEEH_H

#include "neverd/loader/ExceptionCommon.h"
#include "neverd/loader/ExceptionModel.h"
#include "neverd/loader/LanguageEHARM.h"
#include "neverd/loader/LanguageEHCompact.h"
#include "neverd/loader/LanguageEHDelphi.h"
#include "neverd/loader/LanguageEHDwarf.h"
#include "neverd/loader/LanguageEHGo.h"
#include "neverd/loader/LanguageEHItanium.h"
#include "neverd/loader/LanguageEHObjC.h"
#include "neverd/loader/LanguageEHRegistration.h"
#include "neverd/loader/LanguageEHRust.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#endif // NEVERD_LOADER_LANGUAGEEH_H
