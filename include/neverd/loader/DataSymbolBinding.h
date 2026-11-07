//===- DataSymbolBinding.h - Symbol-bound data pointer storage --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_LOADER_DATASYMBOLBINDING_H
#define NEVERD_LOADER_DATASYMBOLBINDING_H

#include "neverd/Common.h"

#include <map>
#include <optional>
#include <string>

namespace neverd {
struct BinaryImage;
struct Import;

/// A dynamic relocation binds this pointer slot to a symbol's address.
/// Unknown (STT_NOTYPE) symbols remain untyped; TLS and callable resolvers
/// cannot be treated as ordinary object addresses.
struct DataSymbolBinding {
  std::string Name;
  std::optional<va_t> Definition;
  int64_t Addend = 0;
  bool Weak = false;
  bool Immutable = false;
  bool operator==(const DataSymbolBinding &) const = default;
};

/// Builds a snapshot from exact relocation symbol records, independent of
/// section names and symbol-name searches: a linked ELF image's data slot
/// relocations, or the GOT cells ObjectExterns gives a relocatable object's
/// undefined data symbols. Conflicting bindings fail clearly.
std::map<va_t, DataSymbolBinding>
collectDataSymbolBindings(const BinaryImage &Image);

/// The pointer-width data words the dynamic loader starts at a function
/// another module defines, by that function's name: an absolute symbol
/// relocation with no addend against a non-weak function symbol, as a pointer
/// variable's initializer (`void (*sink)(int) = exit;`).  The program may
/// store another function there, so the word holds a callable value without
/// being the function's import slot.  A word two such relocations name
/// differently is left out.
std::map<va_t, std::string>
collectExternalFunctionPointers(const BinaryImage &Image);

/// The routine a function-pointer variable starts at: another module's
/// function, by name (collectExternalFunctionPointers), or this image's code,
/// at the address a relocated code pointer holds.  The variable has the type
/// of that routine, so a call through it passes the arguments the routine
/// takes; it never takes the routine's name or contract, since the program may
/// store another routine there.
struct PointerVariableCallee {
  std::string ImportName;
  va_t LocalEntry = InvalidVA;
};

/// Each pointer-width word outside import storage that the loader starts at
/// a routine, by the word's address.
std::map<va_t, PointerVariableCallee>
collectPointerVariableCallees(const BinaryImage &Image);

/// The routine of this image the import \p Imp reaches: the loader starts
/// the slot of a symbol the image defines itself, as the ELF PLT or GOT slot
/// of a preemptible function, at that routine instead of binding it to
/// another module's (collectPointerVariableCallees keys the slot to it).
/// Only the loader writes such a slot, so a call through it, or through the
/// stub that jumps through it, is a call to that routine under the name the
/// slot is bound by.
std::optional<va_t> ownRoutineOfImport(const BinaryImage &Image,
                                       const Import &Imp);
} // namespace neverd
#endif
