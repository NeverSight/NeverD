//===- LocalTypesDebugContext.h - Debug info from local types ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A program's debug information completed from its local types, which are
/// the authority on its records:
/// - the signatures it lacks: a PDB of public symbols names NtCreateFile but
///   gives it no type, where the Windows kernel's headers declare its
///   prototype.  A function takes the prototype of the name it is displayed
///   under, which the program states (its export, else its symbol) or else
///   the debug information gives, with or without debug information; a
///   typed declaration of the program's own stays.  Where a linker folded
///   identical functions, one entry has the names of each, and takes the
///   shown name's prototype only if every other name's that has one passes
///   and returns alike: ExFreePool's one parameter is not ExFreePoolWithTag's
///   two, so which the code reads is unknown;
/// - the members of its records, which the decompiler names its accesses
///   by, so that a member the user renames reads by its new name;
/// - the C that defines those records, which a decompiled unit holds;
/// - the declarations the user gives functions, by their addresses, as
///   IDA's Y does: these come first, before the debug information's.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_LOCALTYPESDEBUGCONTEXT_H
#define NEVERD_SDK_CAPI_LOCALTYPESDEBUGCONTEXT_H

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/debug/DebugContext.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/types/HeaderLibrary.h"
#include "neverd/types/LocalTypes.h"
#include "neverd/types/TypeRender.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace neverd::sdk {

/// \p Type as the decompiler's types describe it: records by their name with
/// their members' names for display, typedefs as what they name, pointers
/// to functions as pointers.  Null for a type it cannot describe, such as a
/// function by value or a record larger than a display offset reaches.
TypeRef decompilerTypeOf(const types::TypeLibrary &Library,
                         const types::TypeExpr &Type);

/// A call-site value keeps a complete natural C record layout. Packed,
/// overlapping, bitfield and incomplete records remain unavailable. Pointer
/// pointees retain the ordinary display representation and need no value ABI.
TypeRef sourceCallTypeOf(const types::TypeLibrary &Library,
                         const types::TypeExpr &Type);

/// \p Function's signature, named \p Name, as the decompiler's types
/// describe it. Variadic declarations retain their fixed prefix and ellipsis.
/// A prototype speller keeps scalar C conversions consistent with its
/// declaration. None for a type the decompiler cannot describe.
std::optional<FunctionSym>
signatureOf(const types::TypeLibrary &Library,
            const types::FunctionDef &Function, llvm::StringRef Name,
            const types::TypeSpeller *PrototypeSpeller = nullptr);

/// Whether \p A and \p B take and return alike: as many parameters, each
/// of the same class and size, and returns of the same class and size, as
/// the folded functions one entry names must for its code to follow either.
bool passesAlike(const FunctionSym &A, const FunctionSym &B);

/// The names of the types the decompiler reads with \p Library's
/// prototypes: those their parameters and returns name, and those these
/// name in turn, through typedefs, pointers, arrays and members, whether
/// the library defines them or not.  A change to another type leaves what
/// the prototypes say alone.
llvm::StringSet<> prototypeTypeNames(const types::TypeLibrary &Library);

class LocalTypesDebugContext final : public DebugContext {
public:
  /// \p Inner outlives this; \p Types holds the prototypes and the types
  /// they name.  \p Declarations are the C declarations the user gave the
  /// functions at those addresses, which read against \p Types.  \p Image,
  /// which outlives this too, states the names of its functions, if given;
  /// \p Headers are the header libraries joined to \p Types, which a
  /// prototype names as its source.  \p Recognized names the functions
  /// signatures recognized by their bytes, which nothing else names.
  /// \p EntryType names the type of \p Image's entry point, a pointer to a
  /// function, where its kind's specification gives it one.  \p ProgramMain
  /// is the function the C runtime runs as main (findProgramMain).
  LocalTypesDebugContext(
      const DebugContext &Inner,
      std::shared_ptr<const types::TypeLibrary> Types,
      std::map<va_t, std::string> Declarations = {},
      const BinaryImage *Image = nullptr,
      std::vector<types::HeaderLibrary> Headers = {},
      std::unordered_map<uint64_t, std::string> Recognized = {},
      std::string EntryType = {},
      std::optional<va_t> ProgramMain = std::nullopt,
      std::string EntryDeclaration = {});
  /// Takes back the members and enumerators it gave the debug information's
  /// types, and gives back those they held, so that the context made after
  /// a change to the library gives them anew.
  /// The records are the debug information's, which the session's calls,
  /// one at a time, share: no decompilation reads them meanwhile.
  ~LocalTypesDebugContext() override;

  /// The inner function, its return type and parameters those of the
  /// user's declaration at its entry, else, where the inner context gives
  /// neither, the library's prototype of the name the function at the entry
  /// is displayed under: the one the image states, else the inner one, else
  /// the one signatures recognized it by.  At an import's slot or stub, the
  /// prototype of the routine imported, under its name; at the image's
  /// entry point, the type its kind's specification gives it.
  std::optional<FunctionSym> resolveFunction(va_t Addr) const override;

  std::optional<std::string> functionName(va_t Addr) const override {
    return Inner.functionName(Addr);
  }
  std::optional<VariableSym> resolveVariable(va_t FuncAddr,
                                             int64_t Offset) const override {
    return Inner.resolveVariable(FuncAddr, Offset);
  }
  bool hasAuthenticatedFunctionSignatures() const override {
    return Inner.hasAuthenticatedFunctionSignatures();
  }
  AuthenticatedReturnValueState
  resolveAuthenticatedReturnValueState(va_t Addr) const override {
    return Inner.resolveAuthenticatedReturnValueState(Addr);
  }
  bool hasAuthenticatedObjectExtents() const override {
    return Inner.hasAuthenticatedObjectExtents();
  }
  std::vector<AuthenticatedRecordParameter>
  resolveAuthenticatedRecordParameters(va_t Addr) const override {
    return Inner.resolveAuthenticatedRecordParameters(Addr);
  }
  std::optional<AuthenticatedRecordParameter>
  resolveAuthenticatedRecordReturn(va_t Addr) const override {
    return Inner.resolveAuthenticatedRecordReturn(Addr);
  }
  VariableExtentLookup resolveVariableAt(va_t FuncAddr, va_t UsePC,
                                         int64_t Offset) const override {
    return Inner.resolveVariableAt(FuncAddr, UsePC, Offset);
  }
  std::optional<VariableSym>
  resolveStackPointerVariable(va_t FuncAddr, int64_t Offset) const override {
    return Inner.resolveStackPointerVariable(FuncAddr, Offset);
  }
  VariableExtentLookup
  resolveStackPointerVariableAt(va_t FuncAddr, va_t UsePC,
                                int64_t Offset) const override {
    return Inner.resolveStackPointerVariableAt(FuncAddr, UsePC, Offset);
  }
  VariableExtentLookup
  resolveFramePointerVariableAt(va_t FuncAddr, va_t UsePC,
                                int64_t Offset) const override {
    return Inner.resolveFramePointerVariableAt(FuncAddr, UsePC, Offset);
  }
  /// A record's members for display from the library, where it defines a
  /// record of the name and size, and an enumeration's enumerators where it
  /// defines one of the name: the library is the authority on the names, so
  /// that one the user renames reads by its new name.  The debug
  /// information's stay on a record the library does not lay out where they
  /// lie, such as a C++ class whose bases the PDB flattens into it.
  void completeType(const TypeRef &Type) const override;
  std::optional<TypeSym> resolveType(uint64_t TypeId) const override {
    return Inner.resolveType(TypeId);
  }
  std::optional<SourceLoc> sourceLocation(va_t Addr) const override {
    return Inner.sourceLocation(Addr);
  }
  std::vector<FunctionSym> allFunctions() const override {
    return Inner.allFunctions();
  }
  std::vector<DataObjectSym> allDataObjects() const override {
    return Inner.allDataObjects();
  }
  std::optional<DataObjectSym> resolveDataObject(va_t Addr) const override {
    return Inner.resolveDataObject(Addr);
  }
  std::pair<std::optional<va_t>, std::optional<va_t>>
  dataSymbolsAround(va_t Addr) const override {
    return Inner.dataSymbolsAround(Addr);
  }
  std::optional<va_t> precedingDataSymbol(va_t Addr) const override {
    return Inner.precedingDataSymbol(Addr);
  }
  bool hasInfo() const override {
    return Inner.hasInfo() || !Declarations.empty();
  }

  /// The signature the library gives \p Name: its return type and named
  /// parameters, the library that declares it and the declaration.  None
  /// for a name without a prototype or a type the decompiler cannot describe.
  std::optional<FunctionSym>
  resolvePrototype(llvm::StringRef Name) const override;

  /// The signature of the user's declaration of the function at \p Entry.
  /// None where the user declared none, or one that no longer reads against
  /// the library, or has a type the decompiler cannot describe.
  std::optional<FunctionSym> declared(va_t Entry) const;

  /// The C that defines \p Records for a decompiled unit
  /// (CEmitterOptions::RecordDefinitions): each one the library defines, of
  /// the name and size the unit reads, whose members the unit names as the
  /// library does, and what these hold by value.  A record of no size or
  /// members is a name the unit spells: a record's or an enumeration's of
  /// the library's, or an enumerator of one the decompiler displays, which
  /// stands for that enumeration.  What the debug information gave types of
  /// the name, members or enumerators, must agree with the library too.
  /// A pointer request obtains only a record's tag, without completing it.
  CRecordDefinitions definitions(const std::vector<TypeRef> &Records) const;
  /// The copy of the library this context reads, which its definitions
  /// render.
  const types::TypeLibrary &library() const { return *Types; }

  /// The library's types the decompiler has read through this context: the
  /// records whose members it gave and the types units held.  A change to
  /// one changes what the decompiler prints.
  llvm::StringSet<> readTypes() const;

  /// Reads the names the image states for its functions (statedNames()),
  /// where nothing waits for them, as warming the analysis does.
  void indexStatedNames() const;
  /// Reads what the record definitions of every unit read (definitions()),
  /// where nothing waits for it, as warming the analysis does.
  void prepareDefinitions() const;

private:
  /// The record the library defines as \p Name, of \p Size bytes unless
  /// that is unknown.
  const types::LocalType *definedRecord(llvm::StringRef Name,
                                        uint16_t Size) const;
  /// Gives \p Type, a record or an enumeration of the debug information,
  /// the library's members or enumerators of its name, the members only
  /// where each one it has lies where one of the library's does.  False
  /// where the library has none to give.  Holds Mutex.
  bool takeLibraryNames(const TypeRef &Type) const;
  /// The names the image states for the function at \p Entry, the one it
  /// is shown under first (BinaryImage::statedFunctionNamesAt).
  std::vector<std::string> statedNames(va_t Entry) const;
  /// The name of the import a call to \p Addr reaches through its slot or
  /// stub (importCalleeName); empty for none.
  std::string importName(va_t Addr) const;
  /// The header library the prototype \p Function of \p Name comes from;
  /// none for one of the local types the user declared.
  std::optional<types::HeaderLibrary>
  prototypeHeader(llvm::StringRef Name,
                  const types::FunctionDef *Function) const;

  const DebugContext &Inner;
  std::shared_ptr<const types::TypeLibrary> Types;
  std::map<va_t, std::string> Declarations;
  const BinaryImage *Image;
  std::vector<types::HeaderLibrary> Headers;
  std::unordered_map<uint64_t, std::string> Recognized;
  /// The type of the image's entry point, a pointer to a function.
  std::string EntryType;
  std::string EntryDeclaration;
  /// The function type EntryType points to, as a signature.
  std::optional<FunctionSym> entrySignature() const;
  std::optional<va_t> ProgramMain;
  /// Main as the C runtime calls it (libc::kProgramMainDeclaration).
  std::optional<FunctionSym> mainSignature() const;
  mutable std::mutex Mutex;
  mutable llvm::StringMap<std::optional<FunctionSym>> Signatures;
  /// What spells the prototypes' declarations, by the spelling of builtins
  /// their libraries' headers use: spelling one alone would name every type
  /// of the libraries joined again.
  mutable std::map<types::BuiltinSpelling, std::unique_ptr<types::TypeSpeller>>
      Spellers;
  mutable std::unique_ptr<types::TypeSpeller> PortablePrototypeSpeller;
  /// What renders the records a unit embeds: spelling the library's names
  /// for each unit would name every type again.
  mutable std::unique_ptr<types::EmbeddedCRenderer> CRenderer;
  /// The names the image states for each function entry that has any, read
  /// at the first statedNames(): one pass over the image's exports and
  /// symbols, where reading them for each entry scans them all again.
  mutable std::optional<std::unordered_map<va_t, std::vector<std::string>>>
      StatedNames;
  /// Fills StatedNames.  Holds Mutex.
  void readStatedNames() const;
  /// Fills RenamedInC.  Holds Mutex.
  void readRenamedInC() const;
  /// Makes CRenderer.  Holds Mutex.
  void makeCRenderer() const;
  mutable std::map<va_t, std::string> ImportNames;
  mutable std::map<va_t, std::optional<FunctionSym>> DeclaredSignatures;
  /// A type of the debug information it gave the library's names, with
  /// the names the type held before, which it takes back: none for a record
  /// the debug information had given no members.
  struct Named {
    TypeRef Type;
    std::vector<std::string> Names;
    std::vector<uint16_t> Offsets;
    std::vector<TypeRef> Types;
    uint16_t Alignment;
  };
  mutable std::vector<Named> Completed;
  mutable llvm::DenseSet<const NdType *> CompletedRecords;
  mutable llvm::StringSet<> Read;
  /// The records and enumerations of the library's names that the debug
  /// information gave members or enumerators, by name.
  mutable llvm::StringMap<std::vector<TypeRef>> Given;
  mutable llvm::DenseSet<const NdType *> GivenTypes;
  /// The library's names C spells otherwise, which a unit cannot name.
  mutable std::optional<llvm::StringSet<>> RenamedInC;
};

} // namespace neverd::sdk

#endif // NEVERD_SDK_CAPI_LOCALTYPESDEBUGCONTEXT_H
