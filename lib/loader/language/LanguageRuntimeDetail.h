//===- LanguageRuntimeDetail.h - Shared symbol spellings -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Internal declarations shared between the image-wide runtime detection pass
/// (LanguageRuntime.cpp) and the personality/routine name classification
/// (LanguagePersonality.cpp).
///
/// This header is an implementation detail of the loader library and should
/// NOT be included by code outside lib/loader/language/.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_LANGUAGE_LANGUAGERUNTIMEDETAIL_H
#define NEVERD_LOADER_LANGUAGE_LANGUAGERUNTIMEDETAIL_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace neverd {

/// Every spelling a platform may give one C symbol, most decorated first.
///
/// A leading underscore is ambiguous: Darwin prefixes every C symbol with
/// one, so `___gxx_personality_v0` and `__gxx_personality_v0` name the same
/// routine, while `_except_handler3` is itself a name whose underscore is
/// part of the source spelling.  Rather than guess which underscore is
/// decoration, every candidate is offered and the caller matches whichever
/// the table knows.
///
/// Defined in LanguagePersonality.cpp.
llvm::SmallVector<llvm::StringRef, 4>
symbolNameCandidates(llvm::StringRef Name);

} // namespace neverd

#endif // NEVERD_LOADER_LANGUAGE_LANGUAGERUNTIMEDETAIL_H
