//===- HighCEmitter.cpp - High IR to C emitter ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Top-level orchestration for the HighIR C emitter: include generation,
/// forward declarations, and per-function dispatch.  Function and statement
/// rendering live in HighCFuncWriter.cpp and HighCStmtWriter.cpp; expression
/// rendering lives in HighCExprWriter.cpp and HighCExprBinOp.cpp.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/c/HighC/HighCEmitter.h"

#include "../../../loader/Swift/SwiftBooleanSourceBinding.h"
#include "../../../loader/Swift/SwiftErrorRuntime.h"
#include "../LibraryPrototype.h"
#include "../UnalignedMemory.h"
#include "../VariadicImportStub.h"
#include "../render/X86FPStateHelpers.h"
#include "HighCWriter.h"

#include "neverd/backend/Limits.h"
#include "neverd/backend/c/FloatingPointContract.h"
#include "neverd/backend/c/render/AArch64PAuthC.h"
#include "neverd/ir/high/CxxObjectActions.h"
#include "neverd/ir/high/HighSwiftErrorProjection.h"
#include "neverd/ir/high/ItaniumHandlers.h"
#include "neverd/ir/high/X86FPStateShape.h"

#define DEBUG_TYPE "neverd-highc-emitter"
#include "neverd/ArchSupport.h"
#include "neverd/Common.h"
#include "neverd/DataAccessWidths.h"
#include "neverd/backend/c/render/HighC/HighCIntrinsicRender.h"
#include "neverd/backend/c/render/SyscallHelpers.h"
#include "neverd/backend/llvm/LLVMX86AddressSpaces.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/MachO/DarwinRuntimeCalls.h"
#include "neverd/loader/StringLiteralSymbols.h"
#include "neverd/loader/SymbolDecoration.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <charconv>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>

namespace neverd {

namespace {

/// Whether one pair of parentheses encloses all of \p Text.
bool enclosed(llvm::StringRef Text) {
  bool Enclosed = Text.size() >= 2 && Text.front() == '(';
  int Depth = 0;
  for (size_t I = 0; Enclosed && I < Text.size(); ++I) {
    const char C = Text[I];
    if (C == '"' || C == '\'') {
      // A literal's parentheses are no grouping.
      for (++I; I < Text.size() && Text[I] != C; ++I)
        if (Text[I] == '\\')
          ++I;
      continue;
    }
    Depth += C == '(' ? 1 : C == ')' ? -1 : 0;
    if (Depth == 0 && I + 1 < Text.size())
      Enclosed = false;
  }
  return Enclosed && Depth == 0;
}

/// `(uintptr_t)` applied to \p Address, keeping one pair of parentheses when
/// a pair already encloses all of it: `(uintptr_t)(frame_base - 80)`. An
/// address that is such a conversion already stays as it is:
/// `(uintptr_t)&stack_storage.var_m50`.
std::string uintptrOf(llvm::StringRef Address) {
  if (c_memory::integerConversion(Address))
    return Address.str();
  if (enclosed(Address))
    return "(uintptr_t)" + Address.str();
  return "(uintptr_t)(" + Address.str() + ")";
}

/// How many pointer words a long image initializer gives each line.
constexpr uint64_t ImageWordsPerLine = 8;
/// The characters a line of a long list or string of image bytes holds at
/// most, and from how many a string's line ends after a NUL or a newline.
constexpr uint64_t ImageLineChars = 96;
constexpr uint64_t ImageStringBreakChars = 48;

/// A byte as an element of a list of image bytes: text as its character, a
/// zero as 0, and any other byte in hexadecimal.
std::string byteListElement(uint8_t Byte) {
  if (!Byte)
    return "0";
  if (Byte == '\'' || Byte == '\\')
    return {'\'', '\\', static_cast<char>(Byte), '\''};
  if (Byte >= 32 && Byte < 127)
    return {'\'', static_cast<char>(Byte), '\''};
  return "0x" + llvm::utohexstr(Byte);
}

unsigned partialIntegerBytes(const TypeRef &Type) {
  if (!Type ||
      (Type->Kind != NdTypeKind::Int && Type->Kind != NdTypeKind::Unknown))
    return 0;
  const unsigned Size = Type->Size;
  return Size && (Size & (Size - 1)) ? Size : 0;
}

void validateAtomicIntegerWidth(const TypeRef &Type) {
  if (partialIntegerBytes(Type))
    llvm::report_fatal_error(
        "HighC atomic access requires a supported machine width");
}

const char *memoryOrderingName(NdMemoryOrdering Ordering) {
  switch (Ordering) {
  case NdMemoryOrdering::None:
    return "plain";
  case NdMemoryOrdering::Relaxed:
    return "relaxed";
  case NdMemoryOrdering::Acquire:
    return "acquire";
  case NdMemoryOrdering::Release:
    return "release";
  case NdMemoryOrdering::AcquireRelease:
    return "acq_rel";
  case NdMemoryOrdering::SequentiallyConsistent:
    return "seq_cst";
  }
  llvm_unreachable("unknown NeverD memory ordering");
}

const char *atomicOrderingToken(NdMemoryOrdering Ordering) {
  switch (Ordering) {
  case NdMemoryOrdering::Relaxed:
    return "__ATOMIC_RELAXED";
  case NdMemoryOrdering::Acquire:
    return "__ATOMIC_ACQUIRE";
  case NdMemoryOrdering::Release:
    return "__ATOMIC_RELEASE";
  case NdMemoryOrdering::AcquireRelease:
    return "__ATOMIC_ACQ_REL";
  case NdMemoryOrdering::SequentiallyConsistent:
    return "__ATOMIC_SEQ_CST";
  case NdMemoryOrdering::None:
    break;
  }
  llvm::report_fatal_error("plain memory access has no atomic order token");
}

const char *atomicCmpXchgFailureOrderingToken(NdMemoryOrdering Ordering) {
  switch (Ordering) {
  case NdMemoryOrdering::Relaxed:
  case NdMemoryOrdering::Release:
    return "__ATOMIC_RELAXED";
  case NdMemoryOrdering::Acquire:
  case NdMemoryOrdering::AcquireRelease:
    return "__ATOMIC_ACQUIRE";
  case NdMemoryOrdering::SequentiallyConsistent:
    return "__ATOMIC_SEQ_CST";
  case NdMemoryOrdering::None:
    break;
  }
  llvm::report_fatal_error("atomic compare-exchange requires memory ordering");
}

void validateAtomicLoadOrdering(NdMemoryOrdering Ordering) {
  if (Ordering == NdMemoryOrdering::Release ||
      Ordering == NdMemoryOrdering::AcquireRelease)
    llvm::report_fatal_error("release ordering is invalid on a load");
}

void validateAtomicStoreOrdering(NdMemoryOrdering Ordering) {
  if (Ordering == NdMemoryOrdering::Acquire ||
      Ordering == NdMemoryOrdering::AcquireRelease)
    llvm::report_fatal_error("acquire ordering is invalid on a store");
}

const char *memoryAddressSpaceName(NdMemoryAddressSpace AddressSpace) {
  switch (AddressSpace) {
  case NdMemoryAddressSpace::Default:
    return "default";
  case NdMemoryAddressSpace::X86FS:
    return "fs";
  case NdMemoryAddressSpace::X86GS:
    return "gs";
  }
  llvm::report_fatal_error("unknown NeverD memory address space");
}

unsigned cMemoryAddressSpace(NdMemoryAddressSpace AddressSpace) {
  if (AddressSpace == NdMemoryAddressSpace::Default)
    llvm::report_fatal_error(
        "default memory does not have a target address-space attribute");
  return llvmX86MemoryAddressSpace(AddressSpace);
}

void validateMemoryAddressSpaceForC(NdMemoryAddressSpace AddressSpace,
                                    Arch TargetArch) {
  if (!isKnownMemoryAddressSpace(AddressSpace))
    llvm::report_fatal_error("unknown NeverD memory address space");
  if (AddressSpace != NdMemoryAddressSpace::Default &&
      TargetArch != Arch::X86 && TargetArch != Arch::X64)
    llvm::report_fatal_error(
        "FS/GS memory address spaces require an x86 C target");
}

bool useMsvcSegmentedRead(const CEmitterOptions &Opts, const HighFunc *Func) {
  // A function Windows' unwind or exception tables describe is Windows code;
  // an ELF function's DWARF FDE describes another platform's.
  return (Func && Func->ExceptionMetadata &&
          !exceptionEncodingIsOutsideWindows(
              Func->ExceptionMetadata->Encoding)) ||
         (Opts.Image && Opts.Image->abiFormat() == BinaryFormat::COFF);
}

std::string memoryHelperName(llvm::StringRef Operation, llvm::StringRef Type,
                             NdMemoryOrdering Ordering,
                             NdMemoryAddressSpace AddressSpace) {
  std::string Name = "neverd_mem_" + Operation.str() + "_";
  if (AddressSpace != NdMemoryAddressSpace::Default)
    Name += std::string(memoryAddressSpaceName(AddressSpace)) + "_";
  if (Ordering != NdMemoryOrdering::None)
    Name += std::string(memoryOrderingName(Ordering)) + "_";
  if (auto Suffix = c_memory::suffix(Type); !Suffix.empty())
    return Name + Suffix;
  if (Type.consume_front("unsigned _BitInt(") && Type.consume_back(")"))
    return Name + "u" + Type.str();
  if (Type.consume_front("_BitInt(") && Type.consume_back(")"))
    return Name + "i" + Type.str();
  return Name + canonicalizeCProjectionIdentifier(Type);
}

bool isBareCIntegerLiteral(llvm::StringRef S) {
  if (S.empty())
    return false;
  if (S.front() == '-')
    S = S.drop_front();
  if (S.empty())
    return false;
  if (S.starts_with("0x") || S.starts_with("0X")) {
    S = S.drop_front(2);
    return !S.empty() && llvm::all_of(S, llvm::isHexDigit);
  }
  return llvm::all_of(S, llvm::isDigit);
}

std::string atomicValueCast(llvm::StringRef Type, llvm::StringRef Val) {
  if (Val.starts_with("(" + Type.str() + ")"))
    return Val.str();
  if (isBareCIntegerLiteral(Val))
    return Val.str();
  return "(" + Type.str() + ")(" + Val.str() + ")";
}

std::string memoryPointerCast(llvm::StringRef Type, llvm::StringRef Address,
                              NdMemoryAddressSpace AddressSpace, bool IsConst) {
  std::string Qualified = IsConst ? "const " : "";
  Qualified += Type.str();
  if (AddressSpace != NdMemoryAddressSpace::Default)
    Qualified += " __attribute__((address_space(" +
                 std::to_string(cMemoryAddressSpace(AddressSpace)) + ")))";
  // addrStr already emits `(uintptr_t)base + imm` for typed pointer offsets.
  // Another integer round-trip is `*(T *)(uintptr_t)((uintptr_t)p + 8)`.
  // `&member` is already a typed object address; do not recast it.  An
  // element of a byte backing (`&table[8]`) is a byte's address.
  if (Address.starts_with("&") && !Address.contains('['))
    return Address.str();
  if (Address.contains("(uintptr_t)"))
    return "(" + Qualified + " *)(" + Address.str() + ")";
  return "(" + Qualified + " *)" + uintptrOf(Address);
}

} // anonymous namespace

namespace {
/// Whether C takes the signature \p Func is emitted with, its debug
/// information's where it has any (\p Declared), for a hosted program's
/// main: an int result and no parameter, or argc and argv, and maybe envp,
/// with no function specifier (C11 5.1.2.2.1, 6.7.4).
bool hasMainSignature(const HighFunc &Func,
                      const std::optional<FunctionSym> &Declared) {
  auto Int = [](const TypeRef &Type) {
    return Type && Type->Kind == NdTypeKind::Int && Type->Size == 4;
  };
  // `char **`: plain char, which a signed or unsigned byte is not.
  auto Strings = [](const TypeRef &Type) {
    return Type && Type->Kind == NdTypeKind::Ptr && Type->Pointee &&
           Type->Pointee->Kind == NdTypeKind::Ptr && Type->Pointee->Pointee &&
           Type->Pointee->Pointee->Kind == NdTypeKind::Int &&
           Type->Pointee->Pointee->Size == 1 &&
           Type->Pointee->Pointee->SourceName == "char";
  };
  if (Func.DoesNotReturn)
    return false;
  std::vector<TypeRef> Params;
  TypeRef Result = Func.ReturnType;
  if (Declared && Declared->ReturnType) {
    Result = Declared->ReturnType;
    for (const auto &[Name, Type] : Declared->Params)
      Params.push_back(Type);
  } else {
    for (const HighParam &Param : Func.Params)
      Params.push_back(Param.Type);
  }
  if (!Int(Result) ||
      (Params.size() != 0 && Params.size() != 2 && Params.size() != 3))
    return false;
  return Params.empty() ||
         (Int(Params[0]) && llvm::all_of(llvm::drop_begin(Params), Strings));
}
} // namespace

bool HighCWriter::takesMainSignature(const HighFunc &Func) const {
  return hasMainSignature(Func, Dbg && Func.Entry
                                    ? Dbg->resolveFunction(Func.Entry)
                                    : std::nullopt);
}

void HighCWriter::prepareFunctionIdentifiers(
    const std::vector<HighFunc> &Funcs) {
  GlobalIdentifierAllocator = CProjectionIdentifierAllocator{};
  // The ordinary identifiers the record definitions declare are the
  // program's names (prepareRecordDefinitions): its typedefs, enumerators,
  // and a header's function named like its record (<sys/sysinfo.h>'s
  // sysinfo), which the unit's own names step around and no typedef of the
  // unit's declares again.
  for (const std::string &Name : RecordDefinitions.Identifiers)
    GlobalIdentifierAllocator.allocateVerbatim(Name);
  for (const std::string &Name : RecordDefinitions.Enumerations)
    GlobalIdentifierAllocator.allocateVerbatim(Name);
  FunctionIdentifiers.clear();
  FunctionSymbolNames.clear();
  FunctionIdentifiersBySourceName.clear();
  ExternalFunctionIdentifiers.clear();
  ExternalCallSources.clear();
  StdcallImportPops = Opts.Image ? c_library::stdcallImportPops(*Opts.Image)
                                 : c_library::ImportPops();
  ExternalSourceIdentifiers.clear();
  DefinedFuncs.clear();
  DefinedFunctionsByIdentifier.clear();
  DefinedFunctionsByAddress.clear();

  // Imported runtime veneers are ordinary discovered functions, and can
  // therefore carry the same source spelling as the external API they jump
  // to.  Give the linked API first choice of that spelling so the veneer gets
  // a distinct C identifier instead of recursively calling itself.
  std::set<std::string> LinkedRuntimeNames;
  std::set<std::string> DarwinLinkNames;
  std::set<const HighExpr *> Seen;
  std::function<void(const ExprPtr &)> Visit = [&](const ExprPtr &Expr) {
    if (!Expr || !Seen.insert(Expr.get()).second)
      return;
    if (Expr->SourceCallHint) {
      const auto &Hint = *Expr->SourceCallHint;
      using Kind = SourceCallTypeHint::Kind;
      std::string Name;
      if (Hint.CallKind == Kind::ObjCRuntimeCall ||
          Hint.CallKind == Kind::SwiftRuntimeCall)
        Name = Hint.TargetName;
      else if (Hint.CallKind == Kind::DarwinRuntimeCall) {
        if (Hint.Signature.Origin ==
                SourceFunctionTypeHint::OriginKind::DarwinSDK ||
            (Hint.Signature.Origin ==
                 SourceFunctionTypeHint::OriginKind::DarwinRuntime &&
             Hint.TargetName == "__isPlatformVersionAtLeast")) {
          Name = "neverd_darwin_" + Hint.TargetName;
          DarwinLinkNames.insert(Hint.TargetName);
        } else
          Name = Hint.TargetName;
      }
      if (Hint.CallKind == Kind::SwiftBooleanProjection)
        Name = swiftBooleanSourceName(Hint.TargetName);
      if (!Name.empty())
        LinkedRuntimeNames.insert(std::move(Name));
    }
    for (const auto &Child : Expr->Operands)
      Visit(Child);
  };
  for (const auto &Func : Funcs)
    walkStmts(Func.Body,
              [&](const HighStmt &Stmt) { forEachExpr(Stmt, Visit); });
  for (const auto &Name : LinkedRuntimeNames) {
    llvm::StringRef RenderedName(Name);
    // Darwin's underscored C runtime APIs already carry their source-level
    // underscore.  Other Mach-O imports have one platform decoration removed
    // before reaching the source-call layer.
    const bool ExactDarwinName = Name == "__stack_chk_fail" ||
                                 llvm::StringRef(Name).starts_with("_Block_");
    if (!ExactDarwinName)
      RenderedName.consume_front("_");
    const auto Identifier =
        GlobalIdentifierAllocator.allocate(RenderedName, "nd_external");
    ExternalFunctionIdentifiers.emplace(Name, Identifier);
    ExternalFunctionIdentifiers.try_emplace(RenderedName.str(), Identifier);
  }
  // The C alias and its assembler link name occupy different namespaces in
  // the declaration, but defining the latter as an ordinary C function still
  // interposes on the imported API. Reserve its exact source spelling too,
  // including underscores that belong to the Darwin API itself.
  for (const auto &Name : DarwinLinkNames)
    GlobalIdentifierAllocator.allocate(Name, "nd_external");

  for (const HighFunc &Func : Funcs) {
    std::string SourceName = Func.Name;
    if (Func.Entry &&
        (SourceName.empty() || isSynthesizedFuncName(SourceName))) {
      if (Dbg) {
        if (auto Sym = Dbg->resolveFunction(Func.Entry);
            Sym && !Sym->Name.empty())
          SourceName = std::move(Sym->Name);
      }
      if ((SourceName.empty() || isSynthesizedFuncName(SourceName)) &&
          Opts.Image) {
        std::string FromImage = Opts.Image->getFunctionNameAt(Func.Entry);
        if (!FromImage.empty() && !isSynthesizedFuncName(FromImage))
          SourceName = std::move(FromImage);
      }
    }
    if (SourceName.empty())
      continue;
    DefinedFuncs[SourceName] = &Func;
    if (SourceName.front() == '_')
      DefinedFuncs[SourceName.substr(1)] = &Func;
    if (Func.Entry) {
      auto [It, Added] = DefinedFunctionsByAddress.emplace(Func.Entry, &Func);
      if (!Added)
        It->second = nullptr;
    }
    const llvm::StringRef SymbolName =
        cNameOfSymbol(SourceName, Opts.Format, Opts.TheArch);
    FunctionSymbolNames.emplace(&Func, SymbolName.str());
    const llvm::StringRef RenderedName =
        cDefinitionName(SymbolName, Opts.Format);
    // The image's own copy of a C library routine steps aside from the
    // header's name, as LLVM-C's does: by the identifier it would take, as a
    // C++ `sigaddset(unsigned long*, int)` would take `sigaddset`, or by its
    // prototype, which a C library's headers give (`__strlen_chk`).
    std::string Stem =
        canonicalizeCProjectionIdentifier(RenderedName, "nd_function");
    if (Func.Entry && (copiesHeaderRoutine(Stem, Opts.Format) ||
                       libraryDeclaresDefinition(Func))) {
      Stem += "_" + llvm::utohexstr(Func.Entry);
      HeaderRoutineCopies.insert(&Func);
    }
    // C holds a whole program's main to the result and parameters C gives
    // main, not those the code has (make's `_Noreturn void main(int32_t,
    // int64_t, void*)`): one whose signature C rejects steps aside as
    // LLVM-C's whole-module main does, the C runtime reaching it by its
    // address.
    else if (Opts.WholeProgram && Func.Entry && Stem == "main" &&
             !takesMainSignature(Func))
      Stem += "_" + llvm::utohexstr(Func.Entry);
    std::string Identifier = GlobalIdentifierAllocator.allocateVerbatim(Stem);
    FunctionIdentifiers.emplace(&Func, Identifier);
    DefinedFunctionsByIdentifier.emplace(Identifier, &Func);
    FunctionIdentifiersBySourceName.try_emplace(SourceName, Identifier);
    FunctionIdentifiersBySourceName.try_emplace(RenderedName.str(), Identifier);
  }

  // Distinct external callees stay distinct.  Symbols whose identifiers are
  // spelled from them can share a stem: Itanium C++ overloads and constructor
  // variants (`_ZN12QDomNodeListC1Ev` and `...C2EP19...` are both
  // `QDomNodeList_ctor`), instances of one Rust generic, Go and GCC names
  // with punctuation.  Each symbol of such a stem takes its own identifier,
  // in encounter order after a symbol that is its own identifier.  MSVC
  // stems merge, as the MSVC rules want.
  std::map<std::string, std::vector<std::string>> SymbolsByStem;
  std::map<std::string, std::set<std::string>> SourcesBySymbol;
  std::set<const HighExpr *> Calls;
  std::function<void(const ExprPtr &)> VisitCall = [&](const ExprPtr &Expr) {
    if (!Expr || !Calls.insert(Expr.get()).second)
      return;
    for (const auto &Child : Expr->Operands)
      VisitCall(Child);
    if (Expr->Kind != ExprKind::Call || Expr->SourceCallHint ||
        Expr->IntrinsicId != Intrinsic::None || Expr->IsIndirectCall)
      return;
    const std::string SourceName = resolvedCallTarget(*Expr);
    if (SourceName.empty() ||
        FunctionIdentifiersBySourceName.count(SourceName) ||
        ExternalFunctionIdentifiers.count(SourceName))
      return;
    const std::string Symbol =
        cNameOfSymbol(SourceName, Opts.Format, Opts.TheArch).str();
    auto &Symbols =
        SymbolsByStem[canonicalizeCProjectionIdentifier(Symbol, "nd_function")];
    if (!llvm::is_contained(Symbols, Symbol))
      Symbols.push_back(Symbol);
    SourcesBySymbol[Symbol].insert(SourceName);
  };
  for (const auto &Func : Funcs)
    walkStmts(Func.Body,
              [&](const HighStmt &Stmt) { forEachExpr(Stmt, VisitCall); });
  for (auto &[Stem, Symbols] : SymbolsByStem) {
    if (Symbols.size() < 2 ||
        llvm::none_of(Symbols, [](const std::string &Symbol) {
          return identifierSpelledFromSymbol(Symbol) && !hasMsvcStem(Symbol);
        }))
      continue;
    std::stable_partition(Symbols.begin(), Symbols.end(),
                          [](const std::string &Symbol) {
                            return !identifierSpelledFromSymbol(Symbol);
                          });
    for (const std::string &Symbol : Symbols) {
      const std::string Identifier =
          GlobalIdentifierAllocator.allocate(Stem, "nd_external");
      ExternalFunctionIdentifiers.emplace(Identifier, Identifier);
      for (const std::string &SourceName : SourcesBySymbol[Symbol])
        ExternalSourceIdentifiers[SourceName] = Identifier;
    }
  }
}

std::string HighCWriter::functionIdentifier(const HighFunc &Func) const {
  if (auto It = FunctionIdentifiers.find(&Func);
      It != FunctionIdentifiers.end())
    return It->second;
  return canonicalizeCProjectionIdentifier(
      cDefinitionName(cNameOfSymbol(Func.Name, Opts.Format, Opts.TheArch),
                      Opts.Format),
      "nd_function");
}

std::string HighCWriter::functionIdentifier(llvm::StringRef SourceName) const {
  if (auto It = FunctionIdentifiersBySourceName.find(SourceName.str());
      It != FunctionIdentifiersBySourceName.end())
    return It->second;
  if (auto It = ExternalSourceIdentifiers.find(SourceName.str());
      It != ExternalSourceIdentifiers.end())
    return It->second;
  if (auto It = ExternalFunctionIdentifiers.find(SourceName.str());
      It != ExternalFunctionIdentifiers.end())
    return It->second;
  // A reference keeps its symbol's exact C name, which it links by.
  std::string Identifier = canonicalizeCProjectionIdentifier(
      cNameOfSymbol(SourceName, Opts.Format, Opts.TheArch), "nd_function");
  ReferencedFunctionSymbols.try_emplace(Identifier, SourceName.str());
  return Identifier;
}

std::string HighCWriter::memoryTypeName(const TypeRef &Ty) const {
  std::string Name = typeToC(Ty);
  return Name == "void" ? "uint32_t" : Name;
}

std::optional<c_float::Conversion>
HighCWriter::floatToIntegerConversion(const HighExpr &E) const {
  if (E.Kind != ExprKind::UnaryOp ||
      (E.Op != NdOp::FLOAT_FLOAT2INT && E.Op != NdOp::FLOAT_FLOAT2UINT &&
       E.Op != NdOp::FLOAT_TRUNC))
    return std::nullopt;
  if (E.Operands.size() != 1 || !E.Operands[0] || !E.Operands[0]->Type ||
      E.Operands[0]->Type->Kind != NdTypeKind::Float || !E.Type ||
      E.Type->Kind != NdTypeKind::Int)
    throw std::runtime_error("unsupported HighC float conversion shape");
  c_float::Conversion Shape{
      unsigned(E.Type->Size * 8), unsigned(E.Operands[0]->Type->Size * 8),
      E.Op != NdOp::FLOAT_FLOAT2UINT, fpToIntegerPolicy(Opts.TheArch)};
  Shape.validate();
  return Shape;
}

void HighCWriter::collectMemoryTypes(const std::vector<HighFunc> &Funcs) {
  std::set<std::string> Names;
  FloatToIntegerHelpers.clear();
  LeadingZeroHelpers.clear();
  SaturatingLaneHelpers.clear();
  ScalarLSEBuiltins.clear();
  PartialIntegerBytes.clear();
  SegmentedMemoryTypes.clear();
  AtomicLoadTypes.clear();
  AtomicStoreTypes.clear();
  HasSegmentedMemory = false;
  NeedsUnalignedTypes = false;
  Has256BitInteger = false;
  Has512BitInteger = false;
  Int128AsBitInt = false;
  X87Helpers.clear();
  X86FPStateHelpers.clear();
  UsesX87Extended = false;
  // A unit can hold two bodies of one entry, and a later unit a body at a
  // freed one's address.
  ParamCacheOf = nullptr;
  EmittedParamIndices.clear();
  // C compilers provide __int128 on targets with 64-bit pointers only.
  const bool TargetHasInt128 = pointerBytes(Opts.TheArch) >= sizeof(uint64_t);
  auto CollectWideType = [&](const TypeRef &Type) {
    if (const unsigned Bytes = partialIntegerBytes(Type))
      PartialIntegerBytes.emplace(typeToC(Type), Bytes);
    // Wide scalar carriers are also used for aggregate results on AArch64.
    // Their typedefs follow the printed integer type, not the source ISA.
    const bool ScalarInteger =
        Type && Type->Kind == NdTypeKind::Int && Type->SourceName.empty();
    Has256BitInteger |= ScalarInteger && Type->Size == 32;
    Has512BitInteger |= ScalarInteger && Type->Size == 64;
    Int128AsBitInt |= !TargetHasInt128 && Type &&
                      Type->Kind == NdTypeKind::Int && Type->Size == 16;
    UsesX87Extended |= Type && Type->Kind == NdTypeKind::Float &&
                       Type->Size == 10 && Type->SourceName.empty();
  };
  std::set<const HighExpr *> Seen;
  bool HideEHRuntimeMemory = false;
  std::function<void(const HighExpr &)> Visit = [&](const HighExpr &E) {
    if (!Seen.insert(&E).second)
      return;
    if (HideEHRuntimeMemory &&
        E.MemoryAddressSpace == NdMemoryAddressSpace::X86FS &&
        !(E.Kind == ExprKind::Call &&
          isX86FPStateMemoryIntrinsic(E.IntrinsicId)))
      return;
    CollectWideType(E.Type);
    CollectWideType(E.CastTo);
    if (const auto Helper = x87HelperFor(E))
      X87Helpers.insert(*Helper);
    if (E.Kind == ExprKind::Call && isX86FPStateIntrinsic(E.IntrinsicId)) {
      const auto Shape = x86FPStateHighShape(E, Opts.TheArch);
      if (!x86FPStateShapeIsValid(E.IntrinsicId, Shape))
        llvm::report_fatal_error("invalid x86 FP state C contract");
      const unsigned Bytes = x86FPStateHelperLayout(E.IntrinsicId, Shape);
      auto [It, Inserted] =
          X86FPStateHelpers.try_emplace(std::pair{E.IntrinsicId, Bytes});
      if (Inserted)
        It->second = GlobalIdentifierAllocator.allocate(
            x86FPStateCHelper(E.IntrinsicId, Bytes), "nd_fp_state");
    }
    if (E.Kind == ExprKind::UnaryOp && E.Op == NdOp::LZCOUNT &&
        !E.Operands.empty() && E.Operands[0]) {
      const unsigned Bits = countedBits(*E.Operands[0]);
      auto [It, Inserted] = LeadingZeroHelpers.try_emplace(Bits);
      if (Inserted && Bits)
        It->second = GlobalIdentifierAllocator.allocate(
            "neverd_clz" + std::to_string(Bits), "nd_clz");
    }
    if (E.Kind == ExprKind::Call && isScalarLSEBuiltin(E.IntrinsicId))
      ScalarLSEBuiltins.insert(E.IntrinsicId);
    if (const auto Bits = x86SaturatingLaneBits(E)) {
      auto [It, Inserted] =
          SaturatingLaneHelpers.try_emplace({E.IntrinsicId, *Bits});
      if (Inserted)
        It->second = GlobalIdentifierAllocator.allocate(
            x86SaturatingLaneHelperName(E.IntrinsicId, *Bits), "nd_saturate");
    }
    if (auto Shape = floatToIntegerConversion(E)) {
      auto [It, Inserted] = FloatToIntegerHelpers.try_emplace(Shape->key());
      if (Inserted) {
        const std::string Name = "neverd_fp_to_" +
                                 std::string(Shape->Signed ? "i" : "u") +
                                 std::to_string(Shape->Bits) + "_f" +
                                 std::to_string(Shape->FloatBits);
        It->second = GlobalIdentifierAllocator.allocate(Name, "nd_fp_convert");
      }
    }
    const bool MsvcSegmentedScalar =
        E.MemoryOrdering == NdMemoryOrdering::None &&
        E.MemoryAddressSpace != NdMemoryAddressSpace::Default &&
        (Opts.TheArch == Arch::X86 || Opts.TheArch == Arch::X64) &&
        E.Kind == ExprKind::Load && useMsvcSegmentedRead(Opts, CurrentFunc);
    if (E.MemoryAddressSpace != NdMemoryAddressSpace::Default) {
      validateMemoryAddressSpaceForC(E.MemoryAddressSpace, Opts.TheArch);
      // A read MSVC's <intrin.h> spells for Windows, as __readgsqword.
      if (!MsvcSegmentedScalar)
        HasSegmentedMemory = true;
      else if (Opts.Format == BinaryFormat::COFF)
        NeedsMsvcIntrinsics = true;
      const bool IsMemoryExpr =
          E.Kind == ExprKind::Load || E.Kind == ExprKind::Store ||
          (E.Kind == ExprKind::BinOp &&
           (E.Op == NdOp::ATOMIC_XCHG || E.Op == NdOp::ATOMIC_ADD ||
            E.Op == NdOp::ATOMIC_CMPXCHG)) ||
          E.Kind == ExprKind::Call;
      if (!IsMemoryExpr)
        llvm::report_fatal_error(
            "HighIR address space is attached to a non-memory expression");
    }
    if (E.Kind == ExprKind::Load) {
      std::string Type = memoryTypeName(E.Type);
      validateMemoryAddressSpaceForC(E.MemoryAddressSpace, Opts.TheArch);
      const bool OrdinaryMemory =
          E.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
          E.MemoryOrdering == NdMemoryOrdering::None;
      NeedsUnalignedTypes |=
          OrdinaryMemory && !c_memory::alias(Type, Opts.ScalarPointers).empty();
      const TypeRef AddressType =
          !E.Operands.empty() && E.Operands[0] ? E.Operands[0]->Type : nullptr;
      const bool DirectTypedLoad =
          OrdinaryMemory && partialIntegerBytes(E.Type) == 0 && AddressType &&
          AddressType->Kind == NdTypeKind::Ptr && AddressType->Pointee &&
          equalSourceTypes(AddressType->Pointee, E.Type);
      bool ExactImageBytes = false;
      if (E.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
          !E.Operands.empty() && E.Operands[0])
        if (auto VA = constAddress(*E.Operands[0]))
          ExactImageBytes = imageBackingAddress(*VA).has_value();
      // Raw machine addresses do not prove C alignment or effective type.
      // Image aliases also require byte-copy helpers to share exact storage.
      if ((!MsvcSegmentedScalar && !DirectTypedLoad) || ExactImageBytes)
        Names.insert(Type);
      if (E.MemoryAddressSpace != NdMemoryAddressSpace::Default &&
          !MsvcSegmentedScalar)
        SegmentedMemoryTypes.insert({Type, E.MemoryAddressSpace});
      if (E.MemoryOrdering != NdMemoryOrdering::None)
        AtomicLoadTypes.insert({Type, E.MemoryOrdering, E.MemoryAddressSpace});
    }
    if (E.Kind == ExprKind::Store && E.Operands.size() >= 2) {
      std::string Type = memoryTypeName(E.Operands[1]->Type);
      validateMemoryAddressSpaceForC(E.MemoryAddressSpace, Opts.TheArch);
      Names.insert(Type);
      NeedsUnalignedTypes |=
          E.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
          E.MemoryOrdering == NdMemoryOrdering::None &&
          !c_memory::alias(Type, Opts.ScalarPointers).empty();
      if (E.MemoryAddressSpace != NdMemoryAddressSpace::Default)
        SegmentedMemoryTypes.insert({Type, E.MemoryAddressSpace});
      if (E.MemoryOrdering != NdMemoryOrdering::None)
        AtomicStoreTypes.insert({Type, E.MemoryOrdering, E.MemoryAddressSpace});
    }
    // An indirect call prints its callee expression too.
    E.forEachChildExpr([&](const ExprPtr &Child) { Visit(*Child); });
  };

  for (const HighFunc &Func : Funcs) {
    CurrentFunc = &Func;
    collectNamedFrameSlots(Func);
    CollectWideType(Func.ReturnType);
    for (const HighParam &Param : Func.Params)
      CollectWideType(Param.Type);
    for (const HighLocal &Local : Func.Locals)
      CollectWideType(Local.Type);
    HideEHRuntimeMemory = hidesRegistrationMemory(Func);
    walkStmts(Func.Body, [&](const HighStmt &Stmt) {
      if (Stmt.MemoryAddressSpace != NdMemoryAddressSpace::Default) {
        if (HideEHRuntimeMemory &&
            Stmt.MemoryAddressSpace == NdMemoryAddressSpace::X86FS)
          return;
        validateMemoryAddressSpaceForC(Stmt.MemoryAddressSpace, Opts.TheArch);
        HasSegmentedMemory = true;
        if (Stmt.Kind != StmtKind::Store)
          llvm::report_fatal_error(
              "HighIR address space is attached to a non-memory statement");
      }
      if (Stmt.Kind == StmtKind::Store && Stmt.StoreVal) {
        std::string Type = memoryTypeName(Stmt.StoreVal->Type);
        validateMemoryAddressSpaceForC(Stmt.MemoryAddressSpace, Opts.TheArch);
        Names.insert(Type);
        NeedsUnalignedTypes |=
            Stmt.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
            Stmt.MemoryOrdering == NdMemoryOrdering::None &&
            !c_memory::alias(Type, Opts.ScalarPointers).empty();
        if (Stmt.MemoryAddressSpace != NdMemoryAddressSpace::Default)
          SegmentedMemoryTypes.insert({Type, Stmt.MemoryAddressSpace});
        if (Stmt.MemoryOrdering != NdMemoryOrdering::None)
          AtomicStoreTypes.insert(
              {Type, Stmt.MemoryOrdering, Stmt.MemoryAddressSpace});
      }
      if (Stmt.Kind == StmtKind::Assign && Stmt.Dst &&
          Stmt.Dst->Kind == ExprKind::Load) {
        const std::string Type = memoryTypeName(Stmt.Dst->Type);
        validateMemoryAddressSpaceForC(Stmt.Dst->MemoryAddressSpace,
                                       Opts.TheArch);
        Names.insert(Type);
        NeedsUnalignedTypes |=
            Stmt.Dst->MemoryAddressSpace == NdMemoryAddressSpace::Default &&
            Stmt.Dst->MemoryOrdering == NdMemoryOrdering::None &&
            !c_memory::alias(Type, Opts.ScalarPointers).empty();
        if (Stmt.Dst->MemoryAddressSpace != NdMemoryAddressSpace::Default)
          SegmentedMemoryTypes.insert({Type, Stmt.Dst->MemoryAddressSpace});
        if (Stmt.Dst->MemoryOrdering != NdMemoryOrdering::None)
          AtomicStoreTypes.insert(
              {Type, Stmt.Dst->MemoryOrdering, Stmt.Dst->MemoryAddressSpace});
      }
      forEachExpr(Stmt, [&](const ExprPtr &E) {
        if (E)
          Visit(*E);
      });
    });
  }
  CurrentFunc = nullptr;
  FrameSlots.clear();

  MemoryTypes = std::move(Names);
}

void HighCWriter::writeMemoryHelpers() {
  for (const auto &[Key, Name] : FloatToIntegerHelpers) {
    auto [Bits, FloatBits, Signed] = Key;
    c_float::writeConversion(
        OS, Name, {Bits, FloatBits, Signed, fpToIntegerPolicy(Opts.TheArch)});
  }
  // The machine counts a zero's leading zeros as its width; C's builtins
  // leave that count undefined.
  for (const auto &[Bits, Name] : LeadingZeroHelpers) {
    if (Name.empty())
      continue;
    const std::string Type = "uint" + std::to_string(Bits) + "_t";
    OS << "static inline int " << Name << "(" << Type << " value) {\n"
       << "    return value ? "
       << (Bits == 32 ? "__builtin_clz" : "__builtin_clzll")
       << "(value) : " << Bits << ";\n}\n\n";
  }
  for (const auto &[Key, Name] : SaturatingLaneHelpers)
    writeX86SaturatingLaneHelper(OS, Name, Key.first, Key.second);
  writeScalarLSEFallbacks(OS, ScalarLSEBuiltins);
  // The accesses' types or assumptions are written only when an access can
  // name a type; otherwise every access keeps its portable byte copy.
  UnalignedTypesWritten = Opts.UseUnalignedPointers && NeedsUnalignedTypes;
  if (UnalignedTypesWritten)
    c_memory::writeTypes(OS, Opts.ScalarPointers);
  if (HasSegmentedMemory)
    OS << "#if !defined(__clang__)\n"
          "#error \"segmented-memory output requires Clang target address "
          "spaces\"\n"
          "#endif\n\n";

  auto WriteHelpers = [&](const std::string &Type,
                          NdMemoryAddressSpace AddressSpace) {
    // Plain accesses of other types, and of a bit-precise integer a
    // little-endian target copies inline, need no helper.
    if (AddressSpace == NdMemoryAddressSpace::Default &&
        (!PartialIntegerBytes.count(Type) ||
         inlineMemoryBytes(Type).value_or(0)))
      return;
    const auto ReadPtr =
        AddressSpace == NdMemoryAddressSpace::Default
            ? "(const void *)address"
            : memoryPointerCast(Type, "address", AddressSpace, true);
    const auto WritePtr =
        AddressSpace == NdMemoryAddressSpace::Default
            ? "(void *)address"
            : memoryPointerCast(Type, "address", AddressSpace, false);
    const auto Partial = PartialIntegerBytes.find(Type);
    const bool ExactBytes = Partial != PartialIntegerBytes.end();
    const auto Copy = AddressSpace == NdMemoryAddressSpace::Default
                          ? "memcpy"
                          : "__builtin_memcpy";
    const auto LoadName =
        memoryHelperName("load", Type, NdMemoryOrdering::None, AddressSpace);
    const auto StoreName =
        memoryHelperName("store", Type, NdMemoryOrdering::None, AddressSpace);
    // A bit-precise integer can have object padding. Assemble its numeric
    // value from exactly the IR bytes instead of copying sizeof(_BitInt(N)).
    // The generated C uses its target's native memory byte order, just as the
    // ordinary scalar helpers do, without depending on bit-integer layout.
    auto WriteByteIndex = [&](unsigned Bytes) {
      OS << "#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__\n"
         << "        unsigned shift = i * 8;\n"
         << "#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__\n"
         << "        unsigned shift = (" << Bytes << " - 1 - i) * 8;\n"
         << "#else\n#error \"unsupported target byte order\"\n#endif\n";
    };
    OS << "static inline " << Type << " " << LoadName
       << "(uintptr_t address) {\n";
    if (ExactBytes) {
      const unsigned Bytes = Partial->second;
      const auto Unsigned = typeToC(NdType::makeInt(Bytes, false));
      OS << "    unsigned char bytes[" << Bytes << "];\n"
         << "    " << Copy << "(bytes, " << ReadPtr << ", " << Bytes << ");\n"
         << "    " << Unsigned << " bits = 0;\n"
         << "    for (unsigned i = 0; i < " << Bytes << "; ++i) {\n";
      WriteByteIndex(Bytes);
      OS << "        bits |= (" << Unsigned << ")bytes[i] << shift;\n"
         << "    }\n"
         << "    return __builtin_bit_cast(" << Type << ", bits);\n";
    } else {
      OS << "    " << Type << " value;\n"
         << "    " << Copy << "(&value, " << ReadPtr << ", sizeof(value));\n"
         << "    return value;\n";
    }
    OS << "}\n\nstatic inline " << Type << " " << StoreName
       << "(uintptr_t address, " << Type << " value) {\n";
    if (ExactBytes) {
      const unsigned Bytes = Partial->second;
      const auto Unsigned = typeToC(NdType::makeInt(Bytes, false));
      OS << "    unsigned char bytes[" << Bytes << "];\n"
         << "    " << Unsigned << " bits = (" << Unsigned << ")value;\n"
         << "    for (unsigned i = 0; i < " << Bytes << "; ++i) {\n";
      WriteByteIndex(Bytes);
      OS << "        bytes[i] = (unsigned char)(bits >> shift);\n"
         << "    }\n"
         << "    " << Copy << "(" << WritePtr << ", bytes, " << Bytes << ");\n";
    } else {
      OS << "    " << Copy << "(" << WritePtr << ", &value, sizeof(value));\n";
    }
    OS << "    return value;\n}\n\n";
  };
  for (const auto &Type : MemoryTypes)
    WriteHelpers(Type, NdMemoryAddressSpace::Default);
  for (const auto &[Type, AddressSpace] : SegmentedMemoryTypes) {
    validateMemoryAddressSpaceForC(AddressSpace, Opts.TheArch);
    WriteHelpers(Type, AddressSpace);
  }

  for (const auto &[Type, Ordering, AddressSpace] : AtomicLoadTypes) {
    if (PartialIntegerBytes.count(Type))
      llvm::report_fatal_error(
          "HighC atomic access requires a supported machine width");
    validateAtomicLoadOrdering(Ordering);
    validateMemoryAddressSpaceForC(AddressSpace, Opts.TheArch);
    OS << "static inline " << Type << " "
       << memoryHelperName("load", Type, Ordering, AddressSpace)
       << "(uintptr_t address) {\n"
       << "    return __atomic_load_n("
       << memoryPointerCast(Type, "address", AddressSpace, true) << ", "
       << atomicOrderingToken(Ordering) << ");\n"
       << "}\n\n";
  }

  for (const auto &[Type, Ordering, AddressSpace] : AtomicStoreTypes) {
    if (PartialIntegerBytes.count(Type))
      llvm::report_fatal_error(
          "HighC atomic access requires a supported machine width");
    validateAtomicStoreOrdering(Ordering);
    validateMemoryAddressSpaceForC(AddressSpace, Opts.TheArch);
    OS << "static inline " << Type << " "
       << memoryHelperName("store", Type, Ordering, AddressSpace)
       << "(uintptr_t address, " << Type << " value) {\n"
       << "    __atomic_store_n("
       << memoryPointerCast(Type, "address", AddressSpace, false) << ", value, "
       << atomicOrderingToken(Ordering) << ");\n"
       << "    return value;\n"
       << "}\n\n";
  }
}

std::string HighCWriter::memoryLoadExpr(const TypeRef &Ty, llvm::StringRef Addr,
                                        NdMemoryOrdering Ordering,
                                        NdMemoryAddressSpace AddressSpace,
                                        bool ExactImageBytes,
                                        MemoryLoadDestination *Destination) {
  validateMemoryAddressSpaceForC(AddressSpace, Opts.TheArch);
  // An image object's bytes are in this program's memory, which no segment
  // register reaches; an atomic access to them keeps the image's alignment
  // (collectImageObjects).
  if (ExactImageBytes && AddressSpace != NdMemoryAddressSpace::Default)
    throw std::invalid_argument("HighC cannot project a segmented image alias");
  std::string Type = memoryTypeName(Ty);
  // MSVC's FS/GS read intrinsics are a useful source-level spelling for
  // Windows targets. Other formats keep the target address-space-qualified
  // helper, so the segment remains explicit in the C memory type.
  if (useMsvcSegmentedRead(Opts, CurrentFunc) &&
      !(CurrentFunc && preservesRegistrationMemory(*CurrentFunc))) {
    if (std::string Seg = renderX86MsvcSegmentedLoad(
            Opts.TheArch, Ty ? Ty->Size : 0, Addr, Ordering, AddressSpace);
        !Seg.empty())
      return Seg;
  }
  if (UnalignedTypesWritten && Ordering == NdMemoryOrdering::None &&
      AddressSpace == NdMemoryAddressSpace::Default)
    if (auto Alias = c_memory::alias(Type, Opts.ScalarPointers); !Alias.empty())
      return "(" + c_memory::access(Alias, Addr) + ")";
  // A bit-precise integer narrower than its storage (an x87 value's ten
  // bytes) fills the low bytes of a little-endian object, so a copy of its
  // bytes is its value; a big-endian target keeps the helper.
  const std::optional<unsigned> Bytes = inlineMemoryBytes(Type);
  if (Ordering == NdMemoryOrdering::None &&
      AddressSpace == NdMemoryAddressSpace::Default && Bytes) {
    const std::string Size = *Bytes ? std::to_string(*Bytes) : std::string();
    if (Destination && !Destination->Name.empty()) {
      Destination->Written = true;
      return c_memory::loadCopy(Destination->Name, Addr,
                                *Bytes ? Size
                                       : "sizeof(" + Destination->Name + ")");
    }
    const auto Value = memoryTemporary(Type, "memory_value");
    // Keep the copy at the expression's original evaluation point. Hoisting
    // it above a conditional or loop would execute guarded loads eagerly.
    return "(" +
           c_memory::loadCopy(Value, Addr,
                              *Bytes ? Size : "sizeof(" + Value + ")") +
           ", " + Value + ")";
  }
  auto It = MemoryTypes.find(Type);
  if (It == MemoryTypes.end())
    llvm::report_fatal_error("HighC memory load type was not collected");
  if (Ordering != NdMemoryOrdering::None)
    validateAtomicLoadOrdering(Ordering);
  return memoryHelperName("load", Type, Ordering, AddressSpace) + "(" +
         uintptrOf(Addr) + ")";
}

std::string HighCWriter::memoryStoreExpr(const TypeRef &Ty,
                                         llvm::StringRef Addr,
                                         llvm::StringRef Val,
                                         NdMemoryOrdering Ordering,
                                         NdMemoryAddressSpace AddressSpace,
                                         bool ExactImageBytes) {
  validateMemoryAddressSpaceForC(AddressSpace, Opts.TheArch);
  // An image object's bytes are in this program's memory, which no segment
  // register reaches; an atomic access to them keeps the image's alignment
  // (collectImageObjects).
  if (ExactImageBytes && AddressSpace != NdMemoryAddressSpace::Default)
    throw std::invalid_argument("HighC cannot project a segmented image alias");
  std::string Type = memoryTypeName(Ty);
  const std::string Value = Ty && Ty->Kind == NdTypeKind::Ptr
                                ? "(" + Type + ")(uintptr_t)(" + Val.str() + ")"
                                : Val.str();
  // A machine address does not establish C alignment or effective type.
  if (UnalignedTypesWritten && Ordering == NdMemoryOrdering::None &&
      AddressSpace == NdMemoryAddressSpace::Default)
    if (auto Alias = c_memory::alias(Type, Opts.ScalarPointers); !Alias.empty())
      return "(" + c_memory::access(Alias, Addr) + " = " + Value + ")";
  const std::optional<unsigned> Bytes = inlineMemoryBytes(Type);
  if (Ordering == NdMemoryOrdering::None &&
      AddressSpace == NdMemoryAddressSpace::Default && Bytes) {
    const auto Address = memoryTemporary("uintptr_t", "memory_address");
    const auto Stored = memoryTemporary(Type, "memory_value");
    return "(" + Address + " = " + uintptrOf(Addr) + ", " + Stored + " = " +
           Value + ", " +
           c_memory::storeCopy(Address, Stored,
                               *Bytes ? std::to_string(*Bytes)
                                      : "sizeof(" + Stored + ")") +
           ", " + Stored + ")";
  }
  auto It = MemoryTypes.find(Type);
  if (It == MemoryTypes.end())
    llvm::report_fatal_error("HighC memory store type was not collected");
  if (Ordering != NdMemoryOrdering::None)
    validateAtomicStoreOrdering(Ordering);
  return memoryHelperName("store", Type, Ordering, AddressSpace) + "(" +
         uintptrOf(Addr) + ", " + Value + ")";
}

std::optional<unsigned>
HighCWriter::inlineMemoryBytes(llvm::StringRef Type) const {
  const auto Partial = PartialIntegerBytes.find(Type.str());
  if (Partial == PartialIntegerBytes.end())
    return 0;
  // A little-endian target stores the value's bytes first in the object.
  if (archLittleEndian(Opts.TheArch))
    return Partial->second;
  return std::nullopt;
}

std::string HighCWriter::statementText(std::string Text) {
  // An assignment through an access needs no parentheses as a statement:
  // `*(_QWORD *)p = 0;`.
  return llvm::StringRef(Text).starts_with("(*(")
             ? c_memory::unparenthesized(Text)
             : Text;
}

std::string HighCWriter::memoryTemporary(llvm::StringRef Type,
                                         llvm::StringRef Base) {
  const auto Name = MemoryIdentifiers.allocate(Base);
  MemoryTemporaries.emplace(Name, Type.str());
  return Name;
}

void HighCWriter::writeMemoryStore(const TypeRef &Ty, llvm::StringRef Addr,
                                   llvm::StringRef Val,
                                   NdMemoryOrdering Ordering,
                                   NdMemoryAddressSpace AddressSpace,
                                   bool ExactImageBytes, int Indent) {
  const std::string Type = memoryTypeName(Ty);
  if (Ordering != NdMemoryOrdering::None ||
      AddressSpace != NdMemoryAddressSpace::Default ||
      PartialIntegerBytes.count(Type) ||
      (UnalignedTypesWritten &&
       !c_memory::alias(Type, Opts.ScalarPointers).empty())) {
    emitIndent(Indent);
    OS << statementText(memoryStoreExpr(Ty, Addr, Val, Ordering, AddressSpace,
                                        ExactImageBytes))
       << ";\n";
    return;
  }
  // A fresh carrier cannot overlap the destination, even for a self-store
  // through an escaped source local. Its initialization also applies the
  // store's exact-width conversion before copying bytes.
  const auto Address = MemoryIdentifiers.allocate("memory_address");
  const auto Value = MemoryIdentifiers.allocate("memory_value");
  emitIndent(Indent);
  OS << "{\n";
  emitIndent(Indent + 1);
  OS << "uintptr_t " << Address << " = " << uintptrOf(Addr) << ";\n";
  emitIndent(Indent + 1);
  OS << Type << " " << Value << " = ";
  if (Ty && Ty->Kind == NdTypeKind::Ptr)
    OS << "(" << Type << ")(uintptr_t)(" << Val << ")";
  else
    OS << Val;
  OS << ";\n";
  emitIndent(Indent + 1);
  OS << c_memory::storeCopy(Address, Value, "sizeof(" + Value + ")") << ";\n";
  emitIndent(Indent);
  OS << "}\n";
}

std::string
HighCWriter::atomicExchangeExpr(const TypeRef &Ty, llvm::StringRef Addr,
                                llvm::StringRef Val, NdMemoryOrdering Ordering,
                                NdMemoryAddressSpace AddressSpace) const {
  if (Ordering == NdMemoryOrdering::None)
    llvm::report_fatal_error("atomic exchange requires memory ordering");
  validateAtomicIntegerWidth(Ty);
  validateMemoryAddressSpaceForC(AddressSpace, Opts.TheArch);
  std::string Type = memoryTypeName(Ty);
  return "__atomic_exchange_n(" +
         memoryPointerCast(Type, Addr, AddressSpace, false) + ", " +
         atomicValueCast(Type, Val) + ", " + atomicOrderingToken(Ordering) +
         ")";
}

std::string
HighCWriter::atomicFetchAddExpr(const TypeRef &Ty, llvm::StringRef Addr,
                                llvm::StringRef Val, NdMemoryOrdering Ordering,
                                NdMemoryAddressSpace AddressSpace) const {
  if (Ordering == NdMemoryOrdering::None)
    llvm::report_fatal_error("atomic fetch-add requires memory ordering");
  validateAtomicIntegerWidth(Ty);
  validateMemoryAddressSpaceForC(AddressSpace, Opts.TheArch);
  std::string Type = memoryTypeName(Ty);
  return "__atomic_fetch_add(" +
         memoryPointerCast(Type, Addr, AddressSpace, false) + ", " +
         atomicValueCast(Type, Val) + ", " + atomicOrderingToken(Ordering) +
         ")";
}

std::string HighCWriter::atomicCompareExchangeExpr(
    const TypeRef &Ty, llvm::StringRef Addr, llvm::StringRef Expected,
    llvm::StringRef Desired, NdMemoryOrdering Ordering,
    NdMemoryAddressSpace AddressSpace) const {
  if (Ordering == NdMemoryOrdering::None)
    llvm::report_fatal_error(
        "atomic compare-exchange requires memory ordering");
  validateAtomicIntegerWidth(Ty);
  validateMemoryAddressSpaceForC(AddressSpace, Opts.TheArch);
  std::string Type = memoryTypeName(Ty);
  return "({ " + Type +
         " neverd_expected = " + atomicValueCast(Type, Expected) +
         "; (void)__atomic_compare_exchange_n(" +
         memoryPointerCast(Type, Addr, AddressSpace, false) +
         ", &neverd_expected, " + atomicValueCast(Type, Desired) + ", 0, " +
         atomicOrderingToken(Ordering) + ", " +
         atomicCmpXchgFailureOrderingToken(Ordering) + "); neverd_expected; })";
}

void HighCWriter::collectCallTargetsExpr(const HighExpr &Expr,
                                         std::set<std::string> &Targets) {
  std::set<const HighExpr *> Seen;
  std::function<void(const HighExpr &)> Visit = [&](const HighExpr &Ex) {
    if (!Seen.insert(&Ex).second)
      return;
    if (Ex.Kind == ExprKind::Call) {
      if (Ex.SourceCallHint) {
        const auto &Hint = *Ex.SourceCallHint;
        const bool DeclaredC =
            (Hint.CallKind == SourceCallTypeHint::Kind::DarwinRuntimeCall ||
             Hint.CallKind ==
                 SourceCallTypeHint::Kind::RuntimeCFunctionAddress) &&
            (Hint.Signature.Origin ==
                 SourceFunctionTypeHint::OriginKind::DarwinSDK ||
             (Hint.Signature.Origin ==
                  SourceFunctionTypeHint::OriginKind::DarwinRuntime &&
              Hint.TargetName == "__isPlatformVersionAtLeast"));
        const bool ClassReferenceAddress =
            Hint.CallKind ==
                SourceCallTypeHint::Kind::RuntimeClassReferenceAddress ||
            Hint.CallKind ==
                SourceCallTypeHint::Kind::RuntimeMetaclassReferenceAddress;
        if (DeclaredC && (Hint.ByteCount == 48 || Hint.ByteCount == 128) &&
            darwinIndirectAffineTransformSignature(Opts.TheArch,
                                                   Hint.TargetName))
          NeedsDarwinAffineTransformBridge = true;
        if (Hint.CallKind == SourceCallTypeHint::Kind::DarwinRuntimeCall &&
            !DeclaredC) {
          if (Hint.TargetName == "__stack_chk_fail")
            NeedsDarwinStackFailure = true;
          else if (llvm::StringRef(Hint.TargetName).starts_with("_Block_"))
            NeedsDarwinBlocks = true;
          else
            NeedsDarwinLocks = true;
        } else if (Hint.CallKind ==
                   SourceCallTypeHint::Kind::DarwinRuntimeGlobalAddress) {
          if (Hint.Signature.Origin ==
                  SourceFunctionTypeHint::OriginKind::DarwinSDK ||
              Hint.Signature.Origin ==
                  SourceFunctionTypeHint::OriginKind::SwiftRuntime) {
            if (!SourceRuntimeDataIdentifiers.count(Hint.TargetName))
              SourceRuntimeDataIdentifiers.emplace(
                  Hint.TargetName,
                  GlobalIdentifierAllocator.allocate(
                      "neverd_darwin_data_" + Hint.TargetName, "nd_data"));
            const auto [Weak, Added] = SourceRuntimeDataWeakImports.emplace(
                Hint.TargetName, Hint.WeakImport);
            if (!Added && Weak->second != Hint.WeakImport)
              ConflictingSourceRuntimeDataIdentities.insert(Hint.TargetName);
          } else
            NeedsDarwinStackGuard |= Hint.TargetName == "__stack_chk_guard";
        } else if (Hint.CallKind ==
                   SourceCallTypeHint::Kind::SwiftStringBridge) {
          NeedsSwiftStringBridge = true;
        } else if (Hint.CallKind ==
                   SourceCallTypeHint::Kind::SwiftBooleanProjection) {
          if (isSwiftBooleanSourceBinding(Hint))
            SwiftBooleanProjectionImports.insert(Hint.TargetName);
        } else if (Hint.CallKind ==
                   SourceCallTypeHint::Kind::SwiftStringFromNSString) {
          NeedsSwiftStringFromNSString = true;
        } else if (Hint.CallKind ==
                   SourceCallTypeHint::Kind::SwiftValueWitness) {
          // The expression reloads the required witness from its runtime
          // metadata argument and therefore needs no linked declaration.
        } else if (Hint.CallKind == SourceCallTypeHint::Kind::SwiftVirtual) {
          // The proven target is a local value loaded before the call.
        } else if (Hint.CallKind ==
                   SourceCallTypeHint::Kind::CFunctionParameterCall) {
          // This invokes the original C function-pointer parameter. It has
          // neither an external symbol nor Objective-C dispatch machinery.
        } else if (Hint.CallKind ==
                       SourceCallTypeHint::Kind::RuntimeCFunctionAddress ||
                   Hint.CallKind == SourceCallTypeHint::Kind::Native ||
                   Hint.CallKind == SourceCallTypeHint::Kind::ObjCRuntimeCall ||
                   Hint.CallKind ==
                       SourceCallTypeHint::Kind::RuntimeObjCSuperGetter ||
                   Hint.CallKind ==
                       SourceCallTypeHint::Kind::RuntimeObjCMergedSetter ||
                   Hint.CallKind ==
                       SourceCallTypeHint::Kind::RuntimeObjCMetadataFactory ||
                   Hint.CallKind == SourceCallTypeHint::Kind::
                                        RuntimeObjCForwardedInitializer ||
                   Hint.CallKind ==
                       SourceCallTypeHint::Kind::SwiftRuntimeCall ||
                   DeclaredC) {
          const bool Runtime =
              Hint.CallKind ==
                  SourceCallTypeHint::Kind::RuntimeCFunctionAddress ||
              Hint.CallKind == SourceCallTypeHint::Kind::ObjCRuntimeCall ||
              Hint.CallKind == SourceCallTypeHint::Kind::SwiftRuntimeCall ||
              Hint.CallKind ==
                  SourceCallTypeHint::Kind::RuntimeObjCSuperGetter ||
              Hint.CallKind ==
                  SourceCallTypeHint::Kind::RuntimeObjCMergedSetter ||
              Hint.CallKind ==
                  SourceCallTypeHint::Kind::RuntimeObjCMetadataFactory ||
              Hint.CallKind ==
                  SourceCallTypeHint::Kind::RuntimeObjCForwardedInitializer ||
              DeclaredC;
          // Scalar ABI declarations use private C identifiers and exact linker
          // names, avoiding conflicting SDK typedefs or libc header prototypes.
          const std::string DeclaredName =
              DeclaredC ? "neverd_darwin_" + Hint.TargetName : "";
          std::string ResolvedName = Runtime || Ex.CallTarget.empty()
                                         ? Hint.TargetName
                                         : Ex.CallTarget;
          if (DeclaredC)
            ResolvedName = DeclaredName;
          if (isSwiftWillThrowSourceCall(Hint, Opts.TheArch))
            ResolvedName = SwiftWillThrowValueSourceName;
          bool IsDefinedIdentifier = false;
          if (!Runtime)
            if (const auto *Definition =
                    sourceCallDefinition(Hint, ResolvedName)) {
              ResolvedName = functionIdentifier(*Definition);
              IsDefinedIdentifier = true;
            }
          llvm::StringRef Name(ResolvedName);
          if (!IsDefinedIdentifier)
            Name.consume_front("_");
          if (!Name.empty()) {
            if (Runtime) {
              const auto [Effect, Fresh] =
                  SourceCallTermination.emplace(Name.str(), Hint.DoesNotReturn);
              if (!Fresh && Effect->second != Hint.DoesNotReturn)
                ConflictingSourceNativeSignatures.insert(Name.str());
              auto [Link, Added] =
                  SourceRuntimeLinkNames.emplace(Name.str(), Hint.TargetName);
              if (!Added && Link->second != Hint.TargetName)
                ConflictingSourceNativeSignatures.insert(Name.str());
            }
            Targets.insert(Name.str());
            const SourceNativeDeclaration Declaration{
                Hint.CallKind ==
                            SourceCallTypeHint::Kind::RuntimeCFunctionAddress &&
                        Hint.AddressedFunctionABI
                    ? &*Hint.AddressedFunctionABI
                    : &Hint.Signature,
                Hint.Format ? std::optional(Hint.Format->FixedCount)
                            : std::nullopt,
                Hint.WeakImport};
            auto [It, Added] =
                SourceNativeSignatures.emplace(Name.str(), Declaration);
            auto TypeSpelling = [](const SourceNativeDeclaration &D) {
              const auto &Signature = *D.Signature;
              std::string Result =
                  sourceConventionAttribute(Signature.Convention).str() +
                  typeToC(Signature.ReturnType) + "(";
              if (D.WeakImport)
                Result += "weak_import,";
              const auto Count =
                  D.VariadicFixedCount.value_or(Signature.Parameters.size());
              if (Count > Signature.Parameters.size())
                return std::string{};
              for (size_t I = 0; I < Count; ++I)
                Result += sourceParameterType(Signature.Parameters[I]) + ",";
              if (D.VariadicFixedCount)
                Result += "...";
              return Result + ")";
            };
            if (!Added && TypeSpelling(It->second) != TypeSpelling(Declaration))
              ConflictingSourceNativeSignatures.insert(Name.str());
          }
        } else if (Hint.CallKind == SourceCallTypeHint::Kind::NativeAddress) {
          if (Hint.TargetAddress)
            if (const auto *Definition = sourceCallDefinition(Hint, {}))
              SourceAddressDefinitions.insert(Definition);
        } else if (Hint.CallKind == SourceCallTypeHint::Kind::RuntimeBlockIsa) {
          llvm::StringRef Name(Hint.TargetName);
          if (Name.starts_with("__"))
            Name = Name.drop_front();
          if (Name == "_NSConcreteStackBlock" ||
              Name == "_NSConcreteGlobalBlock")
            SourceBlockIsaNames.insert(Name.str());
        } else if (Hint.CallKind ==
                       SourceCallTypeHint::Kind::RuntimeBorrowedBytes ||
                   Hint.CallKind ==
                       SourceCallTypeHint::Kind::RuntimeReadOnlyBytes ||
                   Hint.CallKind == SourceCallTypeHint::Kind::
                                        RuntimeSwiftScalarStorageAddress) {
          if (Hint.TargetAddress)
            SourceObjectAddressHelpers.insert(
                "neverd_borrowed_bytes_" +
                llvm::utohexstr(Hint.TargetAddress, true) + "_" +
                std::to_string(Hint.ByteCount) + "_address");
        } else if (Hint.CallKind ==
                   SourceCallTypeHint::Kind::RuntimeCStringStorage) {
          if (Hint.TargetAddress)
            SourceObjectAddressHelpers.insert(
                std::string(Hint.ImmutablePointerSlot
                                ? "neverd_cstring_pointer_"
                                : "neverd_cstring_storage_") +
                llvm::utohexstr(Hint.ImmutablePointerSlot
                                    ? Hint.ImmutablePointerSlot
                                    : Hint.TargetAddress,
                                true) +
                "_address");
        } else if (Hint.CallKind ==
                   SourceCallTypeHint::Kind::RuntimeConstantObjectTable) {
          if (Hint.TargetAddress)
            SourceObjectAddressHelpers.insert(
                "neverd_objc_constant_object_table_" +
                llvm::utohexstr(Hint.TargetAddress, true) + "_address");
        } else if (Hint.CallKind ==
                       SourceCallTypeHint::Kind::RuntimeConstantString ||
                   Hint.CallKind ==
                       SourceCallTypeHint::Kind::RuntimeConstantObject) {
          if (Hint.TargetAddress)
            SourceObjectAddressHelpers.insert(
                std::string(
                    Hint.CallKind ==
                            SourceCallTypeHint::Kind::RuntimeConstantString
                        ? "neverd_objc_constant_string_"
                        : "neverd_objc_constant_object_") +
                llvm::utohexstr(Hint.TargetAddress, true) + "_address");
        } else if (Hint.CallKind ==
                   SourceCallTypeHint::Kind::RuntimeProfileCounterStorage) {
          if (Hint.TargetAddress)
            SourceObjectAddressHelpers.insert(
                "neverd_profile_counters_" +
                llvm::utohexstr(Hint.TargetAddress, true) + "_address");
        } else if (Hint.CallKind ==
                   SourceCallTypeHint::Kind::RuntimeAssociationKey) {
          if (Hint.TargetAddress)
            SourceObjectAddressHelpers.insert(
                "neverd_objc_association_key_" +
                llvm::utohexstr(Hint.TargetAddress, true) + "_address");
        } else if (Hint.CallKind ==
                   SourceCallTypeHint::Kind::RuntimeKVOContext) {
          if (Hint.TargetAddress)
            SourceObjectAddressHelpers.insert(
                "neverd_objc_kvo_context_" +
                llvm::utohexstr(Hint.TargetAddress, true) + "_address");
        } else if (Hint.CallKind ==
                   SourceCallTypeHint::Kind::RuntimeStaticIdentity) {
          if (Hint.TargetAddress)
            SourceObjectAddressHelpers.insert(
                "neverd_static_identity_" +
                llvm::utohexstr(Hint.TargetAddress, true) + "_address");
        } else if (ClassReferenceAddress) {
          if (Hint.TargetAddress)
            SourceObjectAddressHelpers.insert(
                "neverd_objc_class_reference_" +
                llvm::utohexstr(Hint.TargetAddress, true) + "_address");
        } else if (Hint.CallKind ==
                   SourceCallTypeHint::Kind::RuntimeSelectorReferenceAddress) {
          if (Hint.TargetAddress)
            SourceObjectAddressHelpers.insert(
                "neverd_objc_selector_reference_" +
                llvm::utohexstr(Hint.TargetAddress, true) + "_address");
        } else if (Hint.CallKind ==
                   SourceCallTypeHint::Kind::RuntimeLocalStorageAddress) {
          if (Hint.TargetAddress)
            SourceObjectAddressHelpers.insert(
                "neverd_local_storage_" +
                llvm::utohexstr(Hint.TargetAddress, true) + "_address");
        } else if (Hint.CallKind ==
                   SourceCallTypeHint::Kind::RuntimeSwiftSmallStringAddress) {
          if (Hint.TargetAddress)
            SourceObjectAddressHelpers.insert(
                "neverd_swift_small_string_" +
                llvm::utohexstr(Hint.TargetAddress, true) + "_address");
        } else if (Hint.CallKind ==
                   SourceCallTypeHint::Kind::RuntimeSwiftTypeMetadataAddress) {
          if (Hint.SwiftTypeMetadata) {
            const auto &Pair = *Hint.SwiftTypeMetadata;
            const std::string Stem =
                "neverd_swift_type_metadata_" +
                llvm::utohexstr(Pair.CacheAddress, true) + "_" +
                llvm::utohexstr(Pair.ReferenceAddress, true);
            SourceObjectAddressHelpers.insert(Stem + "_cache_address");
            SourceObjectAddressHelpers.insert(Stem + "_reference_address");
          }
        } else if (Hint.CallKind == SourceCallTypeHint::Kind::
                                        RuntimeSwiftNominalDescriptorAddress) {
          if (Hint.TargetAddress)
            SourceObjectAddressHelpers.insert(
                "neverd_swift_nominal_descriptor_" +
                llvm::utohexstr(Hint.TargetAddress, true) + "_address");
        } else if (Hint.CallKind ==
                   SourceCallTypeHint::Kind::
                       RuntimeSwiftConformanceDescriptorAddress) {
          if (Hint.TargetAddress)
            SourceObjectAddressHelpers.insert(
                "neverd_swift_conformance_descriptor_" +
                llvm::utohexstr(Hint.TargetAddress, true) + "_address");
        } else if (Hint.CallKind == SourceCallTypeHint::Kind::
                                        RuntimeSwiftNominalMetadataAddress) {
          if (Hint.TargetAddress)
            SourceObjectAddressHelpers.insert(
                "neverd_swift_nominal_metadata_" +
                llvm::utohexstr(Hint.TargetAddress, true) + "_address");
        } else if (Hint.CallKind ==
                   SourceCallTypeHint::Kind::
                       RuntimeSwiftPrivateNominalMetadataAddress) {
          if (Hint.TargetAddress)
            SourceObjectAddressHelpers.insert(
                "neverd_swift_private_nominal_metadata_" +
                llvm::utohexstr(Hint.TargetAddress, true) + "_address");
        } else if (Hint.CallKind ==
                   SourceCallTypeHint::Kind::RuntimeSwiftWitnessTableAddress) {
          if (Hint.TargetAddress)
            SourceObjectAddressHelpers.insert(
                "neverd_swift_witness_table_" +
                llvm::utohexstr(Hint.TargetAddress, true) + "_address");
        } else if (Hint.CallKind ==
                   SourceCallTypeHint::Kind::RuntimeSwiftWitnessCacheAddress) {
          if (Hint.TargetAddress)
            SourceObjectAddressHelpers.insert(
                "neverd_swift_witness_cache_" +
                llvm::utohexstr(Hint.TargetAddress, true) + "_address");
        } else if (Hint.CallKind ==
                   SourceCallTypeHint::Kind::RuntimeSwiftWitnessAccessor) {
          if (Hint.TargetAddress)
            SourceObjectAddressHelpers.insert(
                "neverd_swift_witness_accessor_" +
                llvm::utohexstr(Hint.TargetAddress, true));
        } else if (Hint.CallKind ==
                   SourceCallTypeHint::Kind::RuntimeSwiftOnceAccessor) {
          if (Hint.TargetAddress)
            SourceObjectAddressHelpers.insert(
                "neverd_swift_once_accessor_" +
                llvm::utohexstr(Hint.TargetAddress, true));
        } else if (Hint.CallKind ==
                       SourceCallTypeHint::Kind::RuntimeBlockDescriptor ||
                   Hint.CallKind ==
                       SourceCallTypeHint::Kind::RuntimeBlockLiteral) {
          if (Hint.TargetAddress)
            SourceObjectAddressHelpers.insert(
                "neverd_block_" +
                std::string(
                    Hint.CallKind ==
                            SourceCallTypeHint::Kind::RuntimeBlockDescriptor
                        ? "descriptor_"
                        : "literal_") +
                llvm::utohexstr(Hint.TargetAddress, true) + "_address");
        } else if (Hint.CallKind == SourceCallTypeHint::Kind::Analyst) {
          if (!Ex.IsIndirectCall)
            Targets.insert(Ex.CallTarget.empty() ? Hint.TargetName
                                                 : Ex.CallTarget);
        } else if (Hint.CallKind != SourceCallTypeHint::Kind::BlockInvoke) {
          NeedsObjCRuntime = true;
          NeedsObjCSuper2 |=
              Hint.CallKind == SourceCallTypeHint::Kind::ObjCSuper2;
        }
        for (const auto &Operand : Ex.Operands)
          if (Operand)
            Visit(*Operand);
        if (Ex.IndirectTarget)
          Visit(*Ex.IndirectTarget);
        return;
      }
      if (Ex.IntrinsicId == Intrinsic::A64_Frinti)
        NeedsFEnvAccess = true;
      const bool IsX87Helper = x87HelperFor(Ex).has_value();
      if (Ex.IntrinsicId == Intrinsic::X64Syscall)
        NeedsX64SyscallHelper = true;
      else if (Ex.IntrinsicId == Intrinsic::X64WindowsSyscall)
        NeedsX64WindowsSyscallHelper = true;
      else if (modeledSyscallConvention(Ex.IntrinsicId))
        LinuxSyscallHelpers.insert(Ex.IntrinsicId);
      // The x87 helpers, collected with the unit's types, need no header.
      else if (Ex.IntrinsicId != Intrinsic::None && !IsX87Helper &&
               (intrinsicCName(Ex.IntrinsicId) ||
                x86MemoryIntrinsicUsesCHeader(Ex.IntrinsicId) ||
                armIntrinsicUsesCHeader(Ex.IntrinsicId)))
        HasCIntrinsics = true;
      if (Opts.Format == BinaryFormat::COFF &&
          (Opts.TheArch == Arch::X86 || Opts.TheArch == Arch::X64) &&
          x86UsesMsvcIntrinsicHeader(Ex.IntrinsicId))
        NeedsMsvcIntrinsics = true;
      if (Opts.Format != BinaryFormat::COFF &&
          (Opts.TheArch == Arch::X86 || Opts.TheArch == Arch::X64) &&
          x86UsesGnuIntrinsicHeader(Ex.IntrinsicId))
        NeedsGnuX86Intrinsics = true;
      // GCC's and Clang's CPUID is <cpuid.h>'s.
      if (Opts.Format != BinaryFormat::COFF &&
          (Opts.TheArch == Arch::X86 || Opts.TheArch == Arch::X64) &&
          Ex.IntrinsicId == Intrinsic::Cpuid)
        NeedsGnuCpuid = true;
      const std::string SourceName = resolvedCallTarget(Ex);
      std::string Name = SourceName;
      if (!Name.empty()) {
        if (Ex.IntrinsicId != Intrinsic::None) {
          CIntrinsicNames.insert(Name);
        }
        if (Ex.IntrinsicId == Intrinsic::None)
          Name = callIdentifier(Ex);
        if (((!isMsvcCxxThrowCallName(Name) &&
              !isMsvcCxxThrowCallName(Ex.CallTarget)) ||
             cxxThrowUsesRuntimeCall(Ex)) &&
            !HiddenCxxCtorIdentifiers.count(Name)) {
          const bool UnresolvedIndirect =
              Ex.IsIndirectCall || Name == "indirect";
          if (!UnresolvedIndirect) {
            ExternalCallSources[Name].insert(SourceName);
            Targets.insert(Name);
            // The statement writer's own rule, read from the call itself: a
            // C name need not be the symbol the rule knows.
            if (isNoreturnCallExpr(Ex))
              NoReturnCallTargets.insert(Name);
            // Only a callee named like a different definition calls through
            // its slot (callIdentifier).
            if (Name != functionIdentifier(SourceName) &&
                Name == importSlotIdentifier(Ex))
              ImportSlotIdentifiers.insert(Name);
            if (auto FS = debugCallee(Ex)) {
              noteDebugExtern(Name, *FS);
              noteDebugExternCallSret(Name, *FS, Ex);
            }
          }
        }
      }
    }
    Ex.forEachChildExpr([&](const ExprPtr &Op) { Visit(*Op); });
  };
  Visit(Expr);
}

void HighCWriter::collectCallTargets(const std::vector<HighStmt> &Stmts,
                                     std::set<std::string> &Targets) {
  for (auto &S : Stmts) {
    if (Analysis.DeadStmts.count(&S) || CxxThrowPrints.count(&S)) {
      collectCallTargets(S.Body, Targets);
      collectCallTargets(S.ElseBody, Targets);
      for (auto &C : S.Cases)
        collectCallTargets(C.Body, Targets);
      collectCallTargets(S.DefaultBody, Targets);
      for (auto &ClauseBody : S.EHClauseBodies)
        collectCallTargets(ClauseBody, Targets);
      continue;
    }
    forEachExpr(S, [&](const ExprPtr &Ex) {
      if (Ex)
        collectCallTargetsExpr(*Ex, Targets);
    });
    collectCallTargets(S.Body, Targets);
    collectCallTargets(S.ElseBody, Targets);
    for (auto &C : S.Cases)
      collectCallTargets(C.Body, Targets);
    collectCallTargets(S.DefaultBody, Targets);
    for (auto &ClauseBody : S.EHClauseBodies)
      collectCallTargets(ClauseBody, Targets);
  }
}

static std::unordered_set<std::string_view>
collectImageFunctionNameViews(const BinaryImage &Image) {
  std::unordered_set<std::string_view> Names;
  Names.reserve(Image.Symbols.size());
  for (const Symbol &Sym : Image.Symbols)
    if (Sym.IsFunc && !Sym.Name.empty() && !Image.findImportAt(Sym.Addr))
      Names.insert(Sym.Name);
  return Names;
}

bool HighCWriter::isOwnFunctionName(llvm::StringRef Name,
                                    const std::vector<HighFunc> &Funcs) {
  llvm::StringRef Clean = Name;
  Clean.consume_front("_");
  for (const HighFunc &F : Funcs) {
    llvm::StringRef Own = F.Name;
    if (Own == Name || Own == Clean || (Own.consume_front("_") && Own == Clean))
      return true;
  }
  if (!Opts.Image)
    return false;
  const auto *Names = SharedImageFunctionNames;
  if (!Names) {
    if (!ImageFunctionNames)
      ImageFunctionNames.emplace(collectImageFunctionNameViews(*Opts.Image));
    Names = &*ImageFunctionNames;
  }
  return Names->count({Name.data(), Name.size()}) ||
         Names->count({Clean.data(), Clean.size()}) ||
         Names->count(("_" + Clean).str());
}

void HighCWriter::writeIncludes(const std::vector<HighFunc> &Funcs) {
  // Block copy/dispose helpers can carry the runtime's opaque
  // _Block_object pointer in debug types. Block.h does not declare this
  // private type, so provide only the incomplete name needed for pointer
  // declarations and casts in a standalone translation unit.
  bool NeedsBlockObject = false;
  bool NeedsBool = false;
  bool HasFloatingArithmetic = false;
  const auto CheckType = [&](const TypeRef &Type) {
    TypeRef Current = Type;
    while (Current && Current->Kind == NdTypeKind::Ptr)
      Current = Current->Pointee;
    NeedsBool |= Current && Current->SourceName == "bool";
    NeedsBlockObject |=
        Current && Current->Kind == NdTypeKind::Struct && !Current->IsEnum &&
        cNamedTypeSpelling(Current->SourceName) == "_Block_object";
  };
  std::set<const HighExpr *> Seen;
  std::function<void(const ExprPtr &)> CheckExpr = [&](const ExprPtr &Expr) {
    if (!Expr || !Seen.insert(Expr.get()).second)
      return;
    CheckType(Expr->Type);
    CheckType(Expr->CastTo);
    HasFloatingArithmetic |=
        Expr->Op == NdOp::FLOAT_ADD || Expr->Op == NdOp::FLOAT_SUB ||
        Expr->Op == NdOp::FLOAT_MULT || Expr->Op == NdOp::FLOAT_DIV;
    if (Expr->Kind == ExprKind::Call) {
      CheckType(knownCallReturnType(*Expr));
      if (Expr->SourceCallHint) {
        CheckType(Expr->SourceCallHint->Signature.ReturnType);
        for (const auto &Parameter : Expr->SourceCallHint->Signature.Parameters)
          CheckType(Parameter.Type);
      }
      for (size_t I = 0;
           I < Expr->Operands.size() && I < debugCallArgLimit(*Expr); ++I)
        CheckType(displayCallArgType(*Expr, I));
    }
    for (const auto &Operand : Expr->Operands)
      CheckExpr(Operand);
  };
  for (const auto &Function : Funcs) {
    CheckType(Function.ReturnType);
    for (const auto &Parameter : Function.Params)
      CheckType(Parameter.Type);
    walkStmts(Function.Body, [&](const HighStmt &Statement) {
      forEachExpr(Statement, CheckExpr);
    });
  }
  // Every spelling of a 16-byte integer, in types and in the intrinsics'
  // casts alike, takes the C23 type a 32-bit target has.
  auto WriteInt128Spelling = [&] {
    if (Int128AsBitInt)
      OS << "#define __int128 _BitInt(128)\n\n";
  };
  if (HasFloatingArithmetic)
    c_float::writeContractionPolicy(OS);
  if (!Opts.EmitIncludes) {
    WriteInt128Spelling();
    if (NeedsBlockObject)
      OS << "typedef struct _Block_object _Block_object;\n\n";
    if (Has256BitInteger)
      OS << "typedef unsigned _BitInt(256) uint256_t;\n"
            "typedef _BitInt(256) int256_t;\n\n";
    if (Has512BitInteger)
      OS << "typedef unsigned _BitInt(512) uint512_t;\n"
            "typedef _BitInt(512) int512_t;\n\n";
    return;
  }

  std::set<std::string> Headers;
  Headers.insert("stdint.h");
  // char16_t and char32_t strings.
  for (const auto &[Addr, Obj] : ImageObjects)
    for (const auto &String : {Obj.String, Obj.PointsTo})
      if (String && String->UnitBytes > 1 && !imageBackingAddress(Addr))
        Headers.insert("uchar.h");
  if (NeedsBool)
    Headers.insert("stdbool.h");
  if (!MemoryTypes.empty())
    Headers.insert("string.h");
  // The objects a C library header declares, which the unit reaches as it
  // declares them, or as bytes at their addresses.
  for (const auto &[Addr, Obj] : ImageObjects)
    if (!Obj.Header.empty() && !imageBackingAddress(Addr))
      Headers.insert(Obj.Header);
  for (const ImageBacking &Backing : ImageBackings)
    if (Backing.Header)
      Headers.insert(ImageObjects.at(Backing.Base).Header);

  std::set<std::string> CallTargets;
  // An analysis-only function prints as ordinary C too, so its callees need
  // the same headers and prototypes.
  for (auto &F : Funcs)
    collectCallTargets(F.Body, CallTargets);
  writeSourceRecordDeclarations(Funcs, CallTargets);
  // A function whose address the code takes is declared as a callee is.
  CallTargets.insert(AddressTakenFunctions.begin(),
                     AddressTakenFunctions.end());

  // A stub of a variadic import passes its arguments on through stdarg.h.
  if (Opts.Image)
    for (const auto &F : Funcs)
      if (const std::string Import =
              c_stub::variadicImportOfStub(*Opts.Image, F.Entry);
          !Import.empty())
        if (const auto *Forward = libc::libcVariadicForward(Import))
          c_stub::addVariadicStubHeaders(Headers, *Forward);
  // So does a function that builds its va_list only to pass it on.
  for (const auto &F : Funcs)
    if (vaStartForwarder(F))
      Headers.insert("stdarg.h");

  for (auto &Name : CallTargets) {
    if (isOwnFunctionName(Name, Funcs))
      continue;
    // A routine the user declares is declared as the user declares it
    // (writeForwardDecls), not by the header that declares the C library's.
    if (userDeclaresCallee(Name))
      continue;
    for (std::string &Header : calleeHeaders(Name))
      Headers.insert(std::move(Header));
  }
  if (NeedsObjCRuntime) {
    Headers.insert("objc/message.h");
    Headers.insert("objc/runtime.h");
  }
  if (NeedsDarwinLocks)
    Headers.insert("os/lock.h");
  if (NeedsDarwinBlocks)
    Headers.insert("Block.h");
  if (NeedsDarwinAffineTransformBridge)
    Headers.insert("string.h");

  if (HasCIntrinsics)
    for (const char *Hdr : getArchIntrinsicHeaders(Opts.TheArch))
      Headers.insert(Hdr);
  if (NeedsMsvcIntrinsics)
    Headers.insert("intrin.h");
  if (NeedsGnuX86Intrinsics)
    Headers.insert("x86intrin.h");
  if (NeedsGnuCpuid)
    Headers.insert("cpuid.h");

  // The headers declare what the unit's types and routines name as NeverD
  // read them: with _GNU_SOURCE, for the GNU C Library, musl and bionic.
  if (libc::headersReadWithGnuSource(Opts.libcVariant()))
    OS << "#define _GNU_SOURCE 1\n";
  for (auto &H : Headers)
    OS << "#include <" << H << ">\n";
  IncludedHeaders = Headers;
  if (NeedsFEnvAccess)
    OS << "#pragma STDC FENV_ACCESS ON\n";
  OS << "\n";
  WriteInt128Spelling();
  if (NeedsBlockObject)
    OS << "typedef struct _Block_object _Block_object;\n\n";
  if (Has256BitInteger)
    OS << "typedef unsigned _BitInt(256) uint256_t;\n"
          "typedef _BitInt(256) int256_t;\n\n";
  if (Has512BitInteger)
    OS << "typedef unsigned _BitInt(512) uint512_t;\n"
          "typedef _BitInt(512) int512_t;\n\n";
  // The program's records the unit names, as its local types define them.
  if (!RecordDefinitions.Text.empty())
    OS << RecordDefinitions.Text << "\n";
  // An enumeration's type is complete only where it is defined, and the
  // unit spells it by its tag's name.  One a header names by a typedef alone
  // has no tag, and the header's typedef is the unit's (<search.h>'s VISIT).
  bool EnumerationTypedefs = false;
  for (const std::string &Name : RecordDefinitions.Enumerations)
    if (!llvm::is_contained(RecordDefinitions.HeaderTypedefs, Name)) {
      OS << "typedef enum " << Name << " " << Name << ";\n";
      EnumerationTypedefs = true;
    }
  if (EnumerationTypedefs)
    OS << "\n";
  // The unit spells the records the definitions name by their names alone
  // too, as where a member it reads gives a local its type, unless an
  // ordinary identifier of the unit's or of a C library header's holds the
  // name, as <time.h>'s timezone does struct timezone's.
  bool Typedefs = false;
  for (const auto &[Tag, Union] : RecordDefinitions.Tags) {
    const std::string Name = cNamedTypeSpelling(Tag);
    if (Name.empty() || RecordTypedefs.count(Name))
      continue;
    if (llvm::is_contained(RecordDefinitions.HeaderTypedefs, Name)) {
      RecordTypedefs.insert(Name);
      continue;
    }
    if (GlobalIdentifierAllocator.holds(Name) || CallTargets.count(Name) ||
        libc::headerOfDataObject(Name) || libc::isKnownFunction(Name)) {
      TagRecords.emplace(Name, Union);
      continue;
    }
    OS << "typedef " << (Union ? "union " : "struct ") << Name << " " << Name
       << ";\n";
    RecordTypedefs.insert(Name);
    Typedefs = true;
  }
  if (Typedefs)
    OS << "\n";
}

void HighCWriter::writeForwardDecls(const std::vector<HighFunc> &Funcs) {
  if (NeedsDarwinStackGuard)
    OS << "extern long __stack_chk_guard[8];\n";
  if (NeedsDarwinStackFailure)
    OS << "extern void __stack_chk_fail(void) __attribute__((noreturn));\n";
  for (auto &F : Funcs) {
    DefinedFuncs[F.Name] = &F;
    // Mach-O object symbols carry one platform decoration underscore.  Calls
    // and definitions both drop it when rendered as C identifiers, so retain
    // that spelling here as well or an internal call is mistaken for an
    // external old-style `int f()` declaration with a conflicting type.
    if (!F.Name.empty() && F.Name[0] == '_')
      DefinedFuncs[F.Name.substr(1)] = &F;
  }

  std::set<std::string> CallTargets;
  // An analysis-only function prints as ordinary C too, so its callees need
  // the same headers and prototypes.
  for (auto &F : Funcs)
    collectCallTargets(F.Body, CallTargets);
  // How many bytes of each callee's result the code reads: a call a
  // statement assigns, its destination's; a call inside an expression, its
  // own type's, or all of it.  A function whose address the code only takes
  // reads none.
  std::map<std::string, uint16_t> ResultBytes;
  std::map<std::string, TypeRef> FloatingResults;
  std::map<std::string, std::vector<uint16_t>> FloatingParameters;
  std::set<std::string> ConflictingFloatingParameters;
  const uint16_t RegisterBytes = pointerBytes(Opts.TheArch);
  for (const HighFunc &F : Funcs)
    walkStmts(F.Body, [&](const HighStmt &S) {
      const HighExpr *Top =
          S.Kind == StmtKind::Call ? S.CallExpr.get() : S.Val.get();
      std::function<void(const HighExpr &)> Visit = [&](const HighExpr &E) {
        if (E.Kind == ExprKind::Call && E.IntrinsicId == Intrinsic::None) {
          uint16_t &Bytes = ResultBytes[callIdentifier(E)];
          if (E.Type && E.Type->Kind == NdTypeKind::Float) {
            auto [It, Inserted] =
                FloatingResults.emplace(callIdentifier(E), E.Type);
            if (!Inserted && It->second->Size != E.Type->Size)
              throw std::runtime_error(
                  "HighC external calls disagree on floating return width");
            const std::string Name = callIdentifier(E);
            std::vector<uint16_t> Parameters;
            for (const auto &Argument : E.Operands)
              if (Argument && Argument->Type &&
                  Argument->Type->Kind == NdTypeKind::Float &&
                  (Argument->Type->Size == sizeof(float) ||
                   Argument->Type->Size == sizeof(double)))
                Parameters.push_back(Argument->Type->Size);
            if (!E.FloatingCallSignature ||
                E.FloatingCallSignature->ReturnBytes != E.Type->Size ||
                Parameters.size() != E.Operands.size() ||
                E.FloatingCallSignature->ParameterBytes != Parameters)
              ConflictingFloatingParameters.insert(Name);
            else {
              const auto [Position, New] =
                  FloatingParameters.emplace(Name, Parameters);
              if (!New && Position->second != Parameters)
                ConflictingFloatingParameters.insert(Name);
            }
          } else
            ConflictingFloatingParameters.insert(callIdentifier(E));
          if (&E != Top)
            Bytes = std::max<uint16_t>(Bytes,
                                       E.Type ? E.Type->Size : RegisterBytes);
          else if (S.Kind == StmtKind::Assign && S.Dst)
            Bytes = std::max<uint16_t>(Bytes, S.Dst->Type ? S.Dst->Type->Size
                                                          : S.Dst->Var.Size);
        }
        E.forEachChildExpr([&](const ExprPtr &Child) { Visit(*Child); });
      };
      forEachExpr(S, [&](const ExprPtr &E) { Visit(*E); });
    });
  // A function whose address the code takes is declared as a callee is.
  CallTargets.insert(AddressTakenFunctions.begin(),
                     AddressTakenFunctions.end());
  UnitCallTargets = CallTargets;

  for (const auto &[Name, Identifier] : SourceRuntimeDataIdentifiers) {
    if (ConflictingSourceRuntimeDataIdentities.count(Name))
      continue;
    OS << "extern ";
    if (SourceRuntimeDataWeakImports.at(Name))
      OS << "__attribute__((weak_import)) ";
    OS << "unsigned char " << Identifier << "[] __asm__(\"";
    OS.write_escaped("_" + Name);
    OS << "\");\n";
  }
  if (NeedsObjCSuper2)
    OS << "extern void objc_msgSendSuper2(void);\n";
  for (const auto &Import : SwiftBooleanProjectionImports) {
    const auto Inputs = swiftBooleanRuntimeInputs("_" + Import);
    if (!Inputs)
      throw std::invalid_argument("Unsupported Swift Boolean runtime inputs");
    OS << "extern _Bool " << swiftBooleanSourceName(Import) << "(";
    for (unsigned I = 0; I != Inputs->Parameters.size(); ++I) {
      if (I)
        OS << ", ";
      OS << sourceParameterType(Inputs->Parameters[I]);
    }
    OS << ") __asm__(\"_" << Import << "\") "
       << sourceConventionAttribute(
              SourceFunctionTypeHint::ConventionKind::Swift)
              .rtrim()
       << ";\n";
  }
  if (NeedsSwiftStringBridge)
    OS << "extern void *neverd_swift_string_to_nsstring(uint64_t, void *) "
          "__asm__(\"_$sSS10FoundationE19_bridgeToObjectiveCSo8NSStringCyF\") "
       << sourceConventionAttribute(
              SourceFunctionTypeHint::ConventionKind::Swift)
              .rtrim()
       << ";\n";
  if (NeedsSwiftStringFromNSString)
    OS << "extern unsigned __int128 neverd_nsstring_to_swift_string(void *) "
          "__asm__(\"_$sSS10FoundationE36_"
          "unconditionallyBridgeFromObjectiveCySSSo8NSStringCSgFZ\") "
       << sourceConventionAttribute(
              SourceFunctionTypeHint::ConventionKind::Swift)
              .rtrim()
       << ";\n";
  for (const auto &Name : SourceBlockIsaNames)
    OS << "extern void *" << Name << "[];\n";
  for (const auto &Name : SourceObjectAddressHelpers)
    OS << "extern uintptr_t " << Name << "(void);\n";

  // A source-bound native helper can appear after its caller in the emitted
  // module. Declare its actual recovered function signature before any body.
  std::set<const HighFunc *> Prototyped;
  for (const auto &[Name, Declaration] : SourceNativeSignatures) {
    if (SourceRuntimeLinkNames.count(Name))
      continue;
    const auto *Signature = Declaration.Signature;
    auto Definition = DefinedFunctionsByIdentifier.find(Name);
    if (Definition == DefinedFunctionsByIdentifier.end() ||
        !Prototyped.insert(Definition->second).second)
      continue;
    const auto &Function = *Definition->second;
    CurrentFunc = &Function;
    Analysis = {};
    runAnalysisPasses(Function);
    const auto ReturnType = InferredVoid ? NdType::makeVoid() : FuncReturnType;
    const auto Convention = Function.SourceTypeHint
                                ? Function.SourceTypeHint->Convention
                                : SourceFunctionTypeHint::ConventionKind::C;
    if (typeToC(ReturnType) != typeToC(Signature->ReturnType) ||
        Convention != Signature->Convention)
      ConflictingSourceNativeSignatures.insert(Name);
    OS << sourceConventionAttribute(Convention);
    if (Function.DoesNotReturn)
      OS << "_Noreturn ";
    std::string Declarator = functionIdentifier(Function) + "(";
    const size_t ParamCount = emittedParamCount(Function);
    for (size_t I = 0; I < ParamCount; ++I) {
      if (I)
        Declarator += ", ";
      Declarator +=
          Function.SourceTypeHint &&
                  I < Function.SourceTypeHint->Parameters.size()
              ? sourceParameterType(Function.SourceTypeHint->Parameters[I])
              : declaredAs(emittedParamTypeAt(Function, I), "");
    }
    if (vaStartForwarder(Function) && ParamCount)
      Declarator += ", ...";
    if (ParamCount == 0)
      Declarator += "void";
    OS << declaredAs(ReturnType, Declarator + ")") << ";\n";
  }
  CurrentFunc = nullptr;
  Analysis = {};
  for (const auto *Function : SourceAddressDefinitions) {
    if (!Prototyped.insert(Function).second)
      continue;
    CurrentFunc = Function;
    Analysis = {};
    runAnalysisPasses(*Function);
    const auto ReturnType = InferredVoid ? NdType::makeVoid() : FuncReturnType;
    std::string Declarator = functionIdentifier(*Function) + "(";
    const size_t ParamCount = emittedParamCount(*Function);
    for (size_t I = 0; I < ParamCount; ++I) {
      if (I)
        Declarator += ", ";
      Declarator +=
          Function->SourceTypeHint &&
                  I < Function->SourceTypeHint->Parameters.size()
              ? sourceParameterType(Function->SourceTypeHint->Parameters[I])
              : declaredAs(emittedParamTypeAt(*Function, I), "");
    }
    if (vaStartForwarder(*Function) && ParamCount)
      Declarator += ", ...";
    if (ParamCount == 0)
      Declarator += "void";
    if (Function->SourceTypeHint)
      OS << sourceConventionAttribute(Function->SourceTypeHint->Convention);
    OS << declaredAs(ReturnType, Declarator + ")") << ";\n";
  }
  CurrentFunc = nullptr;
  Analysis = {};

  // Direct calls can precede the callee's body in address order. Declare the
  // recovered internal signature before any body so C does not infer an
  // obsolete implicit int/no-parameter declaration at that call site.  A
  // function whose address the code takes is named before its body the same
  // way.
  for (const auto &[Name, Function] : DefinedFunctionsByIdentifier) {
    if ((!CallTargets.count(Name) &&
         !AddressTakenDefinitions.count(Function)) ||
        !Prototyped.insert(Function).second)
      continue;
    CurrentFunc = Function;
    Analysis = {};
    runAnalysisPasses(*Function);
    const auto ReturnType = InferredVoid ? NdType::makeVoid() : FuncReturnType;
    if (Function->SourceTypeHint)
      OS << sourceConventionAttribute(Function->SourceTypeHint->Convention);
    if (Function->DoesNotReturn)
      OS << "_Noreturn ";
    std::string Declarator = Name + "(";
    const size_t ParamCount = emittedParamCount(*Function);
    for (size_t I = 0; I < ParamCount; ++I) {
      if (I)
        Declarator += ", ";
      Declarator +=
          Function->SourceTypeHint &&
                  I < Function->SourceTypeHint->Parameters.size()
              ? sourceParameterType(Function->SourceTypeHint->Parameters[I])
              : declaredAs(emittedParamTypeAt(*Function, I), "");
    }
    if (vaStartForwarder(*Function) && ParamCount)
      Declarator += ", ...";
    if (ParamCount == 0)
      Declarator += "void";
    OS << declaredAs(ReturnType, Declarator + ")") << ";\n";
  }
  CurrentFunc = nullptr;
  Analysis = {};

  for (auto &Name : CallTargets) {
    if (HiddenCxxCtorIdentifiers.count(Name))
      continue;
    if ((DefinedFuncs.count(Name) ||
         DefinedFunctionsByIdentifier.count(Name)) &&
        !SourceRuntimeLinkNames.count(Name))
      continue;
    // A routine the user declares is declared as the user declares it, under
    // another identifier where a header the unit includes declares the C
    // library's routine of the name.
    const bool UserDeclared = userDeclaresCallee(Name);
    if (libc::isKnownFunction(Name) && !UserDeclared &&
        (!isOwnFunctionName(Name, Funcs) || includesHeaderOf(Name)))
      continue;
    if (CIntrinsicNames.count(Name))
      continue;
    // A compiler builtin (`__builtin_alloca`) needs no declaration, and one
    // would conflict with the compiler's own.
    if (llvm::StringRef(Name).starts_with("__builtin_"))
      continue;

    std::string CleanName = Name;
    if (!CleanName.empty() && CleanName[0] == '_')
      CleanName = CleanName.substr(1);
    if (libc::isKnownFunction(CleanName) && !UserDeclared &&
        (!isOwnFunctionName(Name, Funcs) || includesHeaderOf(CleanName)))
      continue;

    ExternFuncs.insert(Name);
  }

  // A synthetic C++ special-member prototype names its class record. Declare
  // that record as an incomplete type so the prototype and casts to it are
  // valid C, unless the name is already an ordinary identifier.
  std::set<std::string> DeclaredSyntheticRecords;
  auto DeclareSyntheticThis = [&](llvm::StringRef Identifier) {
    const MsvcCallee *Msvc = msvcCallee(Identifier, Opts.Format);
    if (!Msvc)
      return;
    const std::string Record =
        cNamedTypeSpelling(msvcSyntheticRecord(Identifier, *Msvc));
    if (Record.empty() || !DeclaredSyntheticRecords.insert(Record).second)
      return;
    if (ExternFuncs.count(Record) || isOwnFunctionName(Record, Funcs)) {
      TagRecords.emplace(Record, false);
      return;
    }
    OS << "typedef struct " << Record << " " << Record << ";\n";
    RecordTypedefs.insert(Record);
  };
  for (const std::string &Name : ExternFuncs) {
    // CallTargets already contains the C identifier returned by
    // functionIdentifier (or the source-call projection).  Removing another
    // leading underscore here declares a different function from the call.
    llvm::StringRef RenderedName(Name);
    // A call through an import's slot declares the slot, a function pointer
    // that links by its own name; its source names the import, and the thunk
    // named like it, not the slot.
    const bool ImportSlot = ImportSlotIdentifiers.count(Name);
    const auto Existing = ExternalFunctionIdentifiers.find(Name);
    // The header that declares the C library's routine of the name holds
    // the name, where a routine the user declares takes it: the user's
    // links by the symbol instead (LinkLabel).
    if (Existing == ExternalFunctionIdentifiers.end() && !ImportSlot &&
        userDeclaresCallee(Name) &&
        (includesHeaderOf(RenderedName) ||
         includesHeaderOf(RenderedName.ltrim('_'))))
      if (const std::string Held =
              canonicalizeCProjectionIdentifier(RenderedName, "nd_external");
          !GlobalIdentifierAllocator.holds(Held))
        GlobalIdentifierAllocator.allocateVerbatim(Held);
    std::string Identifier =
        Existing != ExternalFunctionIdentifiers.end() ? Existing->second
        : ImportSlot
            ? GlobalIdentifierAllocator.allocateVerbatim(Name)
            : GlobalIdentifierAllocator.allocate(RenderedName, "nd_external");
    ExternalFunctionIdentifiers.emplace(Name, Identifier);
    ExternalFunctionIdentifiers.try_emplace(RenderedName.str(), Identifier);
    // The statement writer ends a path at a call to a known noreturn function
    // (isNoreturnCallExpr); its declaration must say so, or C falls through.
    const bool NoReturn = libc::isNoReturnFunction(Name) ||
                          libc::isNoReturnFunction(Identifier) ||
                          NoReturnCallTargets.count(Name);
    // An import whose identifier is not its symbol links by the symbol, which
    // a comment spells as its language does.
    std::string LinkLabel, LinkComment;
    const std::string Declared =
        ImportSlot ? "(*" + Identifier + ")" : Identifier;
    if (auto Sources = ExternalCallSources.find(Name);
        Sources != ExternalCallSources.end() && !ImportSlot) {
      for (const std::string &SourceName : Sources->second)
        ExternalSourceIdentifiers[SourceName] = Identifier;
      // A label naming a function this file defines would bind the call to
      // that definition, which is not the function it calls.
      if (Sources->second.size() == 1 &&
          !FunctionIdentifiersBySourceName.count(*Sources->second.begin())) {
        const llvm::StringRef CName =
            cNameOfSymbol(*Sources->second.begin(), Opts.Format, Opts.TheArch);
        if (linksByLabel(CName, Identifier)) {
          llvm::raw_string_ostream Label(LinkLabel);
          Label << " __asm__(\"";
          Label.write_escaped(symbolOfCName(CName, Opts.Format, Opts.TheArch));
          Label << "\")";
          // A name that reads as the label spells it needs no comment.
          if (const std::string Readable = demangledComment(CName);
              Opts.EmitComments && !Readable.empty() && Readable != CName)
            LinkComment = " /* " + Readable + " */";
        }
      }
    }
    auto SourceSignature = SourceNativeSignatures.find(Name);
    if (SourceSignature != SourceNativeSignatures.end() &&
        !ConflictingSourceNativeSignatures.count(Name)) {
      const auto &Declaration = SourceSignature->second;
      const auto &Signature = *Declaration.Signature;
      llvm::StringRef AffineName(Name);
      const auto AffineSignature = AffineName.consume_front("neverd_darwin_")
                                       ? darwinIndirectAffineTransformSignature(
                                             Opts.TheArch, AffineName.str())
                                       : std::nullopt;
      if (NeedsDarwinAffineTransformBridge && AffineSignature) {
        if (Declaration.WeakImport || Declaration.VariadicFixedCount ||
            !equalSourceABIs(Signature, *AffineSignature))
          throw std::invalid_argument(
              "Invalid indirect CGAffineTransform source declaration");
        if (AffineName == "CGRectApplyAffineTransform") {
          const auto Record = typeToC(Signature.ReturnType);
          OS << "typedef struct { double a, b, c, d, tx, ty; } "
                "neverd_CGRectApplyAffineTransform_input;\n"
                "extern "
             << Record << " neverd_CGRectApplyAffineTransform_original("
             << Record
             << ", neverd_CGRectApplyAffineTransform_input) "
                "__asm__(\"_CGRectApplyAffineTransform\");\n"
                "static inline "
             << Record << " " << Identifier << "(" << Record
             << " rect, const void *transform) {\n"
                "  neverd_CGRectApplyAffineTransform_input value;\n"
                "  memcpy(&value, transform, sizeof(value));\n"
                "  return neverd_CGRectApplyAffineTransform_original(rect, "
                "value);\n"
                "}\n";
          continue;
        }
        if (AffineName != "CGContextConcatCTM") {
          const auto Record = typeToC(Signature.ReturnType);
          const auto Original = "neverd_" + AffineName.str() + "_original";
          OS << "extern " << Record << " " << Original << "(" << Record;
          for (size_t I = 1; I < Signature.Parameters.size(); ++I)
            OS << ", "
               << (Signature.Parameters[I].Type->Kind == NdTypeKind::Ptr
                       ? Record
                       : "double");
          OS << ") __asm__(\"_" << AffineName << "\");\n"
             << "static inline " << Record << " " << Identifier
             << "(const void *transform";
          for (size_t I = 1; I < Signature.Parameters.size(); ++I)
            if (Signature.Parameters[I].Type->Kind == NdTypeKind::Ptr)
              OS << ", const void *input_" << I;
            else
              OS << ", double value_" << I;
          OS << ") {\n  " << Record << " value;\n"
             << "  memcpy(&value, transform, sizeof(value));\n";
          for (size_t I = 1; I < Signature.Parameters.size(); ++I)
            if (Signature.Parameters[I].Type->Kind == NdTypeKind::Ptr)
              OS << "  " << Record << " value_" << I << ";\n"
                 << "  memcpy(&value_" << I << ", input_" << I
                 << ", sizeof(value_" << I << "));\n";
          OS << "  return " << Original << "(value";
          for (size_t I = 1; I < Signature.Parameters.size(); ++I)
            OS << ", value_" << I;
          OS << ");\n}\n";
          continue;
        }
        OS << "typedef struct { double a, b, c, d, tx, ty; } "
              "neverd_CGAffineTransform;\n"
              "extern void neverd_CGContextConcatCTM_original(void *, "
              "neverd_CGAffineTransform) "
              "__asm__(\"_CGContextConcatCTM\");\n"
              "static inline void "
           << Identifier
           << "(void *context, const void *transform) {\n"
              "  neverd_CGAffineTransform value;\n"
              "  memcpy(&value, transform, sizeof(value));\n"
              "  neverd_CGContextConcatCTM_original(context, value);\n"
              "}\n";
        continue;
      }
      if (hasSwiftErrorResult(Signature) &&
          Signature.Origin ==
              SourceFunctionTypeHint::OriginKind::SwiftRuntime) {
        const auto Expected = swiftWillThrowSourceSignature(Opts.TheArch);
        const auto Link = SourceRuntimeLinkNames.find(Name);
        if (!Expected || !equalSourceABIs(Signature, *Expected) ||
            Declaration.WeakImport || Declaration.VariadicFixedCount ||
            Link == SourceRuntimeLinkNames.end() ||
            Link->second != "swift_willThrow")
          throw std::invalid_argument(
              "Unsupported Swift error-slot declaration");
        const auto Original = GlobalIdentifierAllocator.allocate(
            Identifier + "_original", "nd_error_runtime");
        OS << "extern void " << Original << "("
           << sourceParameterType(Signature.Parameters[0]) << ", "
           << sourceParameterType(Signature.Parameters[1])
           << ") __asm__(\"_swift_willThrow\") __attribute__((swiftcall));\n"
              "static inline void "
           << Identifier
           << "(void *context, void *error) {\n"
              "  void *slot = error;\n"
              "  "
           << Original
           << "(context, &slot);\n"
              "}\n";
        continue;
      }
      std::string Declarator = Identifier + "(";
      const auto Count =
          Declaration.VariadicFixedCount.value_or(Signature.Parameters.size());
      if (Count > Signature.Parameters.size())
        continue;
      for (size_t I = 0; I < Count; ++I) {
        if (I)
          Declarator += ", ";
        Declarator += sourceParameterType(Signature.Parameters[I]);
      }
      if (Declaration.VariadicFixedCount)
        Declarator += ", ...";
      else if (Signature.Parameters.empty())
        Declarator += "void";
      OS << "extern ";
      if (Declaration.WeakImport)
        OS << "__attribute__((weak_import)) ";
      OS << sourceConventionAttribute(Signature.Convention);
      if (auto Effect = SourceCallTermination.find(Name);
          Effect != SourceCallTermination.end() && Effect->second)
        OS << "__attribute__((noreturn)) ";
      OS << declarationToC(Signature.ReturnType, Declarator + ")");
      const auto Link = SourceRuntimeLinkNames.find(Name);
      if (Link != SourceRuntimeLinkNames.end() && Link->second != Identifier) {
        OS << " __asm__(\"";
        OS.write_escaped("_" + Link->second);
        OS << "\")";
      }
      OS << ";\n";
    } else if (!ConflictingSourceNativeSignatures.count(Name) &&
               !ConflictingDebugExternSigs.count(Name) &&
               DebugExternSigs.count(Name)) {
      DeclareSyntheticThis(Identifier);
      // A C library routine is declared as its header declares it where the
      // declaration needs nothing else: a re-spelling from its types would
      // lose `const`, and conflict with the header a unit includes too.
      const FunctionSym &Library = DebugExternSigs[Name];
      if (Opts.EmitIncludes && Library.PrototypeFromCLibrary &&
          Identifier == Name && Declared == Identifier && LinkLabel.empty() &&
          c_library::selfContainedDeclaration(Library.PrototypeDeclaration,
                                              Name))
        OS << "extern " << Library.PrototypeDeclaration;
      else
        OS << debugExternPrototype(Library, Identifier, Name) << LinkLabel;
      if (NoReturn)
        OS << " __attribute__((noreturn))";
      OS << ";" << LinkComment << "\n";
    } else if (!ConflictingSourceNativeSignatures.count(Name) &&
               msvcCallee(Identifier, Opts.Format)) {
      DeclareSyntheticThis(Identifier);
      OS << debugExternPrototype(FunctionSym{}, Identifier) << ";\n";
    } else if (!ConflictingSourceNativeSignatures.count(Name)) {
      // The calls print as many arguments (debugCallArgLimit).  Calls
      // through an i386 stdcall thunk and its slot (`_Validate@8`,
      // `__imp__Validate@8`) name one routine, which pops what both spell.
      const auto Sources = ExternalCallSources.find(Name);
      llvm::StringRef Symbol = Name;
      if (Sources != ExternalCallSources.end() && !Sources->second.empty()) {
        const llvm::StringRef First = *Sources->second.begin();
        const std::optional<unsigned> Pops =
            stdcallArgumentBytes(First, Opts.Format, Opts.TheArch);
        if (Sources->second.size() == 1 ||
            (Pops && llvm::all_of(Sources->second, [&](const auto &Source) {
               return stdcallArgumentBytes(Source, Opts.Format, Opts.TheArch) ==
                      Pops;
             })))
          Symbol = First;
      }
      // A routine no header declares takes the prototype the C library tables
      // give it; its calls convert their arguments to its parameter types.
      if (const libc::LibCPrototype *Prototype = prototypeForSymbol(Symbol)) {
        OS << "extern "
           << c_library::declaration(*Prototype, Declared, Opts.TheArch,
                                     Opts.Format);
      } else {
        const std::string Register =
            "int" + std::to_string(pointerBytes(Opts.TheArch) * 8) + "_t";
        // A callee nothing declares returns its register whole.  An int
        // holds the result while the code reads at most an int of it; a
        // 64-bit pointer read through one would lose its upper half.
        const auto Read = ResultBytes.find(Name);
        const bool WholeRegister =
            Read != ResultBytes.end() && Read->second > sizeof(int32_t);
        const auto Floating = FloatingResults.find(Name);
        // A compiler runtime routine that returns a double word returns it
        // in the register pair (CompilerRuntimeDoubleWord.def): `__udivti3`'s
        // calls read RDX as its high half.
        const bool DoubleWord = libc::returnsDoubleWord(
            cNameOfSymbol(Symbol, Opts.Format, Opts.TheArch),
            pointerBytes(Opts.TheArch));
        const std::string Return =
            Floating != FloatingResults.end() ? typeToC(Floating->second)
            : DoubleWord
                ? (pointerBytes(Opts.TheArch) == 8 ? "unsigned __int128"
                                                   : "uint64_t")
            : WholeRegister ? Register
                            : "int";
        // A stdcall routine pops the arguments its calls push: C compiled
        // from the output leaves them to it, where its parameters fill the
        // slots its decoration spells.
        const auto Known = knownArity(Symbol, Name);
        const std::optional<unsigned> Pops = stdcallPops(Symbol, Name);
        const bool Stdcall = Known && Known->FpArgs == 0 &&
                             Known->IntArgs >= 0 && Pops &&
                             *Pops == static_cast<unsigned>(Known->IntArgs) *
                                          pointerBytes(Opts.TheArch);
        OS << "extern " << (Stdcall ? "__attribute__((stdcall)) " : "")
           << Return << " " << Declared << "(";
        if (Floating != FloatingResults.end() &&
            FloatingParameters.count(Name) &&
            !ConflictingFloatingParameters.count(Name)) {
          const auto &Parameters = FloatingParameters.at(Name);
          if (Parameters.empty())
            OS << "void";
          for (size_t I = 0; I < Parameters.size(); ++I) {
            if (I)
              OS << ", ";
            OS << typeToC(NdType::makeFloat(Parameters[I]));
          }
        } else if (auto Arity = Known;
                   Arity && Arity->FpArgs == 0 && Arity->IntArgs >= 0) {
          // Each argument fills one integer register: an int64_t on a 32-bit
          // target would take two, and the arguments after it would move.
          if (Arity->IntArgs == 0)
            OS << "void";
          else {
            for (int I = 0; I < Arity->IntArgs; ++I) {
              if (I)
                OS << ", ";
              OS << Register;
            }
          }
        }
        OS << ")";
      }
      OS << LinkLabel;
      if (NoReturn)
        OS << " __attribute__((noreturn))";
      OS << ";" << LinkComment << "\n";
    }
  }

  if (!ExternFuncs.empty())
    OS << "\n";

  // An import slot the code reads as data is a pointer the loader fills with
  // the import's address, declared by the name it links as.  A slot a call
  // goes through as well is already the function pointer the call declares.
  ImportDataSlotNames.clear();
  bool DeclaredDataSlot = false;
  for (va_t Slot : ImportDataSlotReads) {
    const std::string Symbol = importDataSlotIdentifier(Slot);
    if (Symbol.empty())
      continue;
    if (ImportSlotIdentifiers.count(Symbol))
      if (const auto Declared = ExternalFunctionIdentifiers.find(Symbol);
          Declared != ExternalFunctionIdentifiers.end()) {
        ImportDataSlotNames.emplace(Slot, Declared->second);
        continue;
      }
    const std::string Identifier =
        GlobalIdentifierAllocator.allocateVerbatim(Symbol);
    OS << "extern void *" << Identifier;
    if (Identifier != Symbol)
      OS << " __asm__(\"" << Symbol << "\")";
    OS << ";\n";
    ImportDataSlotNames.emplace(Slot, Identifier);
    DeclaredDataSlot = true;
  }
  if (DeclaredDataSlot)
    OS << "\n";
}

void HighCWriter::collectImageObjects(const std::vector<HighFunc> &Funcs) {
  ImageObjects.clear();
  ImageBackings.clear();
  DataSymbolBindings = Opts.Image ? collectDataSymbolBindings(*Opts.Image)
                                  : std::map<va_t, DataSymbolBinding>{};
  UsedDataBindings.clear();
  ExternalDataNames.clear();
  ImportDataSlotReads.clear();
  FunctionAddressNames.clear();
  AddressTakenFunctions.clear();
  AddressTakenDefinitions.clear();
  if (!Opts.Image)
    return;
  /// An image load an expression is, through integer views and the locals
  /// assigned one: its address and the bytes it reads.
  struct ImageLoad {
    va_t Addr;
    uint16_t Bytes;
  };
  VarKeyMap<ImageLoad> ImageLoadVars;
  auto imageLoad = [&](const HighExpr &E) -> std::optional<ImageLoad> {
    const HighExpr *Inner = unwrapIntegerView(&E);
    if (!Inner)
      return std::nullopt;
    if (Inner->Kind == ExprKind::Load &&
        Inner->MemoryAddressSpace == NdMemoryAddressSpace::Default &&
        Inner->MemoryOrdering == NdMemoryOrdering::None &&
        !Inner->Operands.empty() && Inner->Operands[0])
      if (const auto VA = constAddress(*Inner->Operands[0]))
        return ImageLoad{
            *VA, static_cast<uint16_t>(Inner->Type ? Inner->Type->Size : 0)};
    if (Inner->Kind == ExprKind::Var || Inner->Kind == ExprKind::Phi) {
      if (auto It = ImageLoadVars.find(varKey(Inner->Var));
          It != ImageLoadVars.end())
        return It->second;
    }
    return std::nullopt;
  };
  auto imageLoadVA = [&](const HighExpr &E) -> std::optional<va_t> {
    if (const auto Load = imageLoad(E))
      return Load->Addr;
    return std::nullopt;
  };
  for (const HighFunc &Func : Funcs)
    walkStmts(Func.Body, [&](const HighStmt &S) {
      if (S.Kind != StmtKind::Assign || !S.Dst || !S.Val)
        return;
      if (S.Dst->Kind != ExprKind::Var && S.Dst->Kind != ExprKind::Phi)
        return;
      if (auto Load = imageLoad(*S.Val))
        ImageLoadVars[varKey(S.Dst->Var)] = *Load;
    });
  // Symbols size the objects; the largest one at an address is the object.
  if (SharedDataObjects) {
    DataObjects = SharedDataObjects;
  } else {
    OwnDataObjects = collectImageDataObjects(*Opts.Image);
    DataObjects = &OwnDataObjects;
  }
  // The function the walk reads, and the statement in it.
  const HighFunc *Reading = nullptr;
  const HighStmt *ReadingStmt = nullptr;
  // Where each statement of the function being read sits: the list holding
  // it, its index there, and the statement holding that list.
  struct StmtPlace {
    const std::vector<HighStmt> *List = nullptr;
    size_t Index = 0;
    const HighStmt *Parent = nullptr;
  };
  std::unordered_map<const HighStmt *, StmtPlace> Places;
  // The statements a goto enters, or that hold one it enters: control
  // reaches them without passing what comes before them in their list.
  std::unordered_set<const HighStmt *> Entered;
  // The assignment of each register and temporary that the function assigns
  // once and never takes the address of.
  std::map<VarKey, const HighStmt *> SingleDefinitions;
  const HighFunc *DefinitionsOf = nullptr;
  // Read once the walk asks about a variable of the function.  Exception
  // dispatch enters handlers and landing pads at addresses no goto names, so
  // a function with any assigns nothing this reads.
  auto ReadDefinitions = [&] {
    if (DefinitionsOf == Reading)
      return;
    DefinitionsOf = Reading;
    Places.clear();
    Entered.clear();
    SingleDefinitions.clear();
    if (!Reading || Reading->ExceptionMetadata)
      return;
    std::set<va_t> Targets;
    bool Dispatched = false;
    walkStmts(Reading->Body, [&](const HighStmt &S) {
      if (S.Kind == StmtKind::Goto)
        Targets.insert(S.GotoTarget);
      Targets.insert(S.CxxContinuationReturnTargets.begin(),
                     S.CxxContinuationReturnTargets.end());
      Dispatched |= !S.EHClauses.empty();
    });
    if (Dispatched)
      return;
    std::map<VarKey, unsigned> Assignments;
    std::set<VarKey> AddressTaken;
    HighExprSet Seen;
    std::function<void(const HighExpr &)> NoteAddresses =
        [&](const HighExpr &E) {
          if (!Seen.insert(&E).second)
            return;
          if (E.Kind == ExprKind::Addr && !E.Operands.empty() &&
              E.Operands[0] &&
              (E.Operands[0]->Kind == ExprKind::Var ||
               E.Operands[0]->Kind == ExprKind::Phi))
            AddressTaken.insert(varKey(E.Operands[0]->Var));
          E.forEachChildExpr([&](const ExprPtr &Child) {
            if (Child)
              NoteAddresses(*Child);
          });
        };
    std::function<bool(const std::vector<HighStmt> &, const HighStmt *)> Place =
        [&](const std::vector<HighStmt> &List, const HighStmt *Parent) {
          bool Holds = false;
          for (size_t I = 0; I < List.size(); ++I) {
            const HighStmt &S = List[I];
            Places[&S] = {&List, I, Parent};
            bool In = S.Addr && Targets.count(S.Addr);
            In |= Place(S.Body, &S);
            In |= Place(S.ElseBody, &S);
            for (const SwitchCase &Case : S.Cases)
              In |= Place(Case.Body, &S);
            In |= Place(S.DefaultBody, &S);
            for (const std::vector<HighStmt> &Body : S.EHClauseBodies)
              In |= Place(Body, &S);
            if (In)
              Entered.insert(&S);
            Holds |= In;
            if (S.Kind == StmtKind::Assign && S.Dst && S.Val &&
                (S.Dst->Kind == ExprKind::Var ||
                 S.Dst->Kind == ExprKind::Phi)) {
              const VarKey Key = varKey(S.Dst->Var);
              if (++Assignments[Key] == 1 && (S.Dst->Var.Kind == MedVar::Reg ||
                                              S.Dst->Var.Kind == MedVar::Temp))
                SingleDefinitions[Key] = &S;
              else
                SingleDefinitions.erase(Key);
            }
            forEachExpr(S, [&](const ExprPtr &E) {
              if (E)
                NoteAddresses(*E);
            });
          }
          return Holds;
        };
    Place(Reading->Body, nullptr);
    for (const VarKey &Key : AddressTaken)
      SingleDefinitions.erase(Key);
  };
  // Whether \p Def runs before \p Use on every path that reaches it: it
  // comes first in a list that holds \p Use, and no goto enters between.
  auto Dominates = [&](const HighStmt &Def, const HighStmt &Use) {
    const auto Defined = Places.find(&Def);
    if (Defined == Places.end())
      return false;
    for (const HighStmt *At = &Use; At;) {
      const auto Place = Places.find(At);
      if (Place == Places.end())
        return false;
      if (Place->second.List == Defined->second.List) {
        if (Place->second.Index <= Defined->second.Index)
          return false;
        for (size_t I = Defined->second.Index + 1; I <= Place->second.Index;
             ++I)
          if (Entered.count(&(*Place->second.List)[I]))
            return false;
        return true;
      }
      At = Place->second.Parent;
    }
    return false;
  };
  // The largest unsigned value an integer expression takes at its width,
  // where its form proves one: a constant, a load's width, a mask, an
  // unsigned quotient or remainder, an extension or truncation of a value
  // whose sign is known, the sums, scales and shifts of those that cannot
  // wrap, and a register or temporary its one assignment before the
  // statement \p At gives.  Any other form proves nothing.
  const auto Ones = [](uint64_t Bytes) -> std::optional<uint64_t> {
    if (!Bytes || Bytes > 8)
      return std::nullopt;
    return Bytes == 8 ? ~uint64_t{0} : (uint64_t{1} << (8 * Bytes)) - 1;
  };
  // Whether \p Value, as a \p Bytes-wide integer, is non-negative.
  const auto Nonnegative = [](uint64_t Value, uint64_t Bytes) {
    return Bytes && Bytes <= 8 && Value < (uint64_t{1} << (8 * Bytes - 1));
  };
  // \p Value, the largest a value of type \p From takes, once the value is
  // converted to \p Bytes: a narrowing keeps its low bytes, and a widening
  // keeps an unsigned or non-negative value.
  const auto Converted = [&](std::optional<uint64_t> Value, const TypeRef &From,
                             uint64_t Bytes) -> std::optional<uint64_t> {
    if (!Value || !From)
      return std::nullopt;
    if (From->Size > Bytes) {
      const auto Top = Ones(Bytes);
      if (!Top)
        return std::nullopt;
      return std::min(*Value, *Top);
    }
    if (From->Size == Bytes || From->Kind == NdTypeKind::Ptr ||
        (From->Kind == NdTypeKind::Int && !From->IsSigned) ||
        Nonnegative(*Value, From->Size))
      return Value;
    return std::nullopt;
  };
  std::function<std::optional<uint64_t>(const HighExpr &, const HighStmt *,
                                        unsigned)>
      Maximum = [&](const HighExpr &E, const HighStmt *At,
                    unsigned Depth) -> std::optional<uint64_t> {
    if (Depth > limits::kMaxIntegerViewUnwrapDepth || !E.Type)
      return std::nullopt;
    const uint64_t Width = E.Type->Size;
    // An operand's largest value as this expression's width reads it.
    const auto Of = [&](const HighExpr *Operand) -> std::optional<uint64_t> {
      if (!Operand || !Operand->Type)
        return std::nullopt;
      return Converted(Maximum(*Operand, At, Depth + 1), Operand->Type, Width);
    };
    const auto Within = [&](uint64_t Value) -> std::optional<uint64_t> {
      const auto Top = Ones(Width);
      if (!Top || Value > *Top)
        return std::nullopt;
      return Value;
    };
    const auto Operand = [&](size_t I) -> const HighExpr * {
      return I < E.Operands.size() ? E.Operands[I].get() : nullptr;
    };
    switch (E.Kind) {
    case ExprKind::Const:
      if (!Nonnegative(E.ConstVal, Width))
        return std::nullopt;
      return E.ConstVal;
    case ExprKind::Var:
    case ExprKind::Phi: {
      if (!At || (E.Var.Kind != MedVar::Reg && E.Var.Kind != MedVar::Temp))
        return std::nullopt;
      ReadDefinitions();
      const auto Definition = SingleDefinitions.find(varKey(E.Var));
      if (Definition == SingleDefinitions.end())
        return std::nullopt;
      const HighStmt &Def = *Definition->second;
      if (!Def.Dst->Type || !Def.Val->Type || Def.Dst->Type->Size != Width ||
          Def.Val->Type->Size != Width || !Dominates(Def, *At))
        return std::nullopt;
      const auto Value = Maximum(*Def.Val, &Def, Depth + 1);
      return Value ? Within(*Value) : std::nullopt;
    }
    case ExprKind::Load:
      if (E.Type->Kind != NdTypeKind::Int && E.Type->Kind != NdTypeKind::Ptr)
        return std::nullopt;
      return Ones(Width);
    case ExprKind::Cast:
    case ExprKind::BitCast: {
      const HighExpr *From = Operand(0);
      if (!From || !From->Type ||
          (From->Type->Kind != NdTypeKind::Int &&
           From->Type->Kind != NdTypeKind::Ptr) ||
          (E.Type->Kind != NdTypeKind::Int && E.Type->Kind != NdTypeKind::Ptr))
        return std::nullopt;
      return Of(From);
    }
    case ExprKind::UnaryOp: {
      const HighExpr *From = Operand(0);
      if (!From || !From->Type)
        return std::nullopt;
      const auto Value = Maximum(*From, At, Depth + 1);
      if (E.Op == NdOp::INT_ZEXT)
        return Value ? Value : Ones(From->Type->Size);
      if (E.Op == NdOp::INT_SEXT && Value &&
          Nonnegative(*Value, From->Type->Size))
        return Value;
      return std::nullopt;
    }
    case ExprKind::BinOp: {
      const HighExpr *Left = Operand(0);
      const HighExpr *Right = Operand(1);
      if (!Left || !Right)
        return std::nullopt;
      const auto RightConst = [&]() -> std::optional<uint64_t> {
        if (Right->Kind != ExprKind::Const)
          return std::nullopt;
        return Right->ConstVal;
      };
      switch (E.Op) {
      case NdOp::INT_AND: {
        const auto L = Of(Left);
        const auto R = Of(Right);
        if (L && R)
          return std::min(*L, *R);
        return L ? L : R;
      }
      case NdOp::INT_ADD: {
        const auto L = Of(Left);
        const auto R = Of(Right);
        if (!L || !R || *L > ~uint64_t{0} - *R)
          return std::nullopt;
        return Within(*L + *R);
      }
      case NdOp::INT_MULT: {
        const auto L = Of(Left);
        const auto R = Of(Right);
        if (!L || !R || (*R && *L > ~uint64_t{0} / *R))
          return std::nullopt;
        return Within(*L * *R);
      }
      case NdOp::INT_LEFT: {
        const auto L = Of(Left);
        const auto Shift = RightConst();
        if (!L || !Shift || *Shift >= 64 || (*L >> (63 - *Shift)) > 1)
          return std::nullopt;
        return Within(*L << *Shift);
      }
      case NdOp::INT_RIGHT: {
        auto L = Of(Left);
        if (!L)
          L = Ones(Width);
        const auto Shift = RightConst();
        if (!L)
          return std::nullopt;
        return Shift && *Shift < 64 ? *L >> *Shift : *L;
      }
      case NdOp::INT_REM:
        if (const auto Divisor = RightConst(); Divisor && *Divisor) {
          const auto L = Of(Left);
          return L ? std::min(*L, *Divisor - 1) : *Divisor - 1;
        }
        return std::nullopt;
      case NdOp::INT_DIV:
        if (const auto Divisor = RightConst(); Divisor && *Divisor) {
          auto L = Of(Left);
          if (!L)
            L = Ones(Width);
          if (!L)
            return std::nullopt;
          return *L / *Divisor;
        }
        return std::nullopt;
      case NdOp::SUBBYTES:
        if (const auto Offset = RightConst(); Offset && *Offset == 0) {
          const auto Top = Ones(Width);
          const auto L = Maximum(*Left, At, Depth + 1);
          if (!Top)
            return std::nullopt;
          return L ? std::min(*L, *Top) : *Top;
        }
        return std::nullopt;
      default:
        return std::nullopt;
      }
    }
    default:
      return std::nullopt;
    }
  };
  // The bytes from \p Base that an access of \p Bytes at \p Address
  // reaches, in whole accesses, when every term of the sum but \p Base
  // proves its range at the statement \p At.
  const unsigned AddressBytes = pointerBytes(Opts.TheArch);
  auto ProvenReach = [&](const HighExpr &Address, va_t Base, uint64_t Bytes,
                         const HighStmt *At) -> std::optional<uint64_t> {
    if (!Bytes || Bytes > (~uint64_t{0} >> 2))
      return std::nullopt;
    uint64_t Offset = 0;
    bool SawBase = false;
    std::function<bool(const HighExpr &, unsigned)> Terms =
        [&](const HighExpr &E, unsigned Depth) {
          if (Depth > limits::kMaxIntegerViewUnwrapDepth)
            return false;
          if (E.Kind == ExprKind::BinOp && E.Op == NdOp::INT_ADD &&
              E.Operands.size() == 2 && E.Operands[0] && E.Operands[1])
            return Terms(*E.Operands[0], Depth + 1) &&
                   Terms(*E.Operands[1], Depth + 1);
          // A view of the whole sum at its own width.
          if ((E.Kind == ExprKind::Cast || E.Kind == ExprKind::BitCast) &&
              E.Type && E.Operands.size() == 1 && E.Operands[0] &&
              E.Operands[0]->Type && E.Operands[0]->Type->Size == E.Type->Size)
            return Terms(*E.Operands[0], Depth + 1);
          if (!SawBase && E.Kind == ExprKind::Const && E.ConstVal == Base) {
            SawBase = true;
            return true;
          }
          const auto Value = Maximum(E, At, 0);
          const uint64_t Width = E.Type ? E.Type->Size : 0;
          if (!Value ||
              (Width != AddressBytes && !Nonnegative(*Value, Width)) ||
              *Value > (~uint64_t{0} >> 2) - Offset)
            return false;
          Offset += *Value;
          return true;
        };
    if (!Terms(Address, 0) || !SawBase)
      return std::nullopt;
    return (Offset + Bytes + Bytes - 1) / Bytes * Bytes;
  };
  // Whether a symbol sizes the object holding \p Addr, or a data symbol
  // begins \p Object, the stretch indexedObjectAt() gives \p Addr.
  auto SymbolBounds = [&](va_t Addr, const std::pair<va_t, uint64_t> &Object) {
    if (sizedObjectAt(Addr))
      return true;
    const auto &Starts = DataObjects->DataSymbolStarts;
    if (std::binary_search(Starts.begin(), Starts.end(), Object.first))
      return true;
    return Dbg && Dbg->dataSymbolsAround(Addr).first == Object.first;
  };
  // A table read or written at offsets whose range the code proves, in a
  // stretch no symbol bounds.  It is declared once the walk has shown what
  // else reaches that stretch.
  struct BoundedAccess {
    va_t Table;
    std::pair<va_t, uint64_t> Stretch;
    uint64_t Reach;
    TypeRef Access;
    bool Written;
  };
  std::vector<BoundedAccess> BoundedAccesses;
  // The stretches the code reaches whole.
  std::set<va_t> WholeStretches;
  // The code reaches all of an object through an address that varies or
  // points into it: the object is declared whole, as bytes.  Where no symbol
  // bounds it, the bytes from \p Addr that the access at \p Address
  // provably reaches may be all it declares (BoundedAccesses).
  auto NoteWhole = [&](va_t Addr, const TypeRef &Access, bool Written,
                       const HighExpr *Address = nullptr) {
    const auto Object = indexedObjectAt(Addr);
    if (!Object)
      return;
    if (Address && Access && Addr - Object->first < Object->second &&
        !SymbolBounds(Addr, *Object))
      if (const auto Reach =
              ProvenReach(*Address, Addr, Access->Size, ReadingStmt);
          Reach && *Reach <= Object->second - (Addr - Object->first)) {
        BoundedAccesses.push_back({Addr, *Object, *Reach, Access, Written});
        return;
      }
    WholeStretches.insert(Object->first);
    noteImageObject(Object->first, Access, Written, Access != nullptr);
    ImageObject &Obj = ImageObjects[Object->first];
    Obj.IndexedBytes = std::max(Obj.IndexedBytes, Object->second);
  };
  // A relative table holds the offsets of its targets from its own start
  // (clang's `.rel` lookup tables), so the code reaches each target through
  // the table's address: the table's object spans the targets.  Code that
  // adds an entry back to the table proves it relative, and so does a loader
  // that relocated every entry as such a displacement
  // (BinaryImage::RelDataPtrRelocSlots); a target of a proven table may be
  // any string, in another section (an object's strings).  Otherwise each
  // entry is an offset the assembler fixed to a sized object or a string of
  // text after the table in its section; a relocated slot of such a table is
  // a pointer.
  auto StringExtent = [&](va_t Target) -> std::optional<uint64_t> {
    for (uint64_t Byte = 0; Byte < limits::kMaxStringScanLen; ++Byte) {
      const uint8_t *Unit = Opts.Image->readVA(Target + Byte, 1);
      if (!Unit || Opts.Image->hasRelocationProvenanceAt(Target + Byte))
        return std::nullopt;
      if (*Unit == 0)
        return Byte + 1;
    }
    return std::nullopt;
  };
  auto TextExtent = [&](va_t Target) -> std::optional<uint64_t> {
    if (const auto Text = imageCString(Opts.Image, Target))
      return Text->Bytes;
    return std::nullopt;
  };
  auto NoteRelativeTargets = [&](va_t Table, uint16_t EntryBytes,
                                 bool AddedBack) {
    auto Object = sizedObjectAt(Table);
    const Section *Sec = Opts.Image->getSectionFor(Table);
    // A table no symbol sizes (clang's `.Lswitch.table`) that code adds its
    // entries back to spans what the code indexes of it.
    if (AddedBack && (!Object || Object->first != Table))
      if (const auto It = ImageObjects.find(Table);
          It != ImageObjects.end() && It->second.IndexedBytes)
        Object = std::make_pair(Table, It->second.IndexedBytes);
    if (!Object || Object->first != Table ||
        (EntryBytes != 4 && EntryBytes != 8))
      return;
    bool Relocated = Object->second >= EntryBytes;
    for (va_t Slot = Table;
         Relocated && Slot + EntryBytes <= Table + Object->second;
         Slot += EntryBytes)
      Relocated = Opts.Image->RelDataPtrRelocSlots.count(Slot) != 0;
    const bool Proven = AddedBack || Relocated;
    if (!Proven &&
        (!Sec || Sec->Size > std::numeric_limits<va_t>::max() - Sec->VA))
      return;
    // A proven table may also reach targets before it (clang puts strings
    // ahead of the table in its section): one object then spans from the
    // first of them, so that the table and its targets keep their distance.
    const va_t TableEnd = Table + Object->second;
    va_t Start = Table, End = TableEnd;
    for (va_t Slot = Table; Slot + EntryBytes <= TableEnd; Slot += EntryBytes) {
      const uint8_t *Bytes = Opts.Image->readVA(Slot, EntryBytes);
      if (!Bytes || (!Proven && Opts.Image->hasRelocationProvenanceAt(Slot)))
        return;
      const int64_t Offset =
          EntryBytes == 4 ? static_cast<int64_t>(
                                static_cast<int32_t>(readLE<uint32_t>(Bytes)))
                          : static_cast<int64_t>(readLE<uint64_t>(Bytes));
      const bool Before = Offset < 0;
      const uint64_t Distance = Before ? uint64_t(0) - uint64_t(Offset)
                                       : static_cast<uint64_t>(Offset);
      if (Before
              ? !Proven || Distance > Table ||
                    Distance > limits::kMaxRelativeTableSpanBytes
              : Distance < Object->second ||
                    (Proven && Distance >= limits::kMaxRelativeTableSpanBytes))
        return;
      const va_t Target = Before ? Table - Distance : Table + Distance;
      va_t TargetEnd = 0;
      if (const auto TargetObject = sizedObjectAt(Target);
          TargetObject && TargetObject->first == Target)
        TargetEnd = Target + TargetObject->second;
      else if (const auto String =
                   Proven ? StringExtent(Target) : TextExtent(Target))
        TargetEnd = Target + *String;
      else
        return;
      if (Before
              ? TargetEnd > Table
              : (Proven ? TargetEnd - Table > limits::kMaxRelativeTableSpanBytes
                        : TargetEnd > Sec->VA + Sec->Size))
        return;
      Start = std::min<va_t>(Start, Target);
      End = std::max<va_t>(End, TargetEnd);
    }
    ImageObject &Obj = ImageObjects[Table];
    Obj.IndexedBytes = std::max<uint64_t>(Obj.IndexedBytes, End - Table);
    if (Start < Table) {
      ImageObject &Span = ImageObjects[Start];
      Span.IndexedBytes = std::max<uint64_t>(Span.IndexedBytes, End - Start);
    }
  };
  // A variable holding an entry of an indexed image table: code that adds
  // it back to the table (`e = table[i]; *(table + e)`) reads a relative
  // table as surely as one that adds the load itself.
  // A variable a function assigns once, a constant address, stands for
  // that address: the code keeps a table's address in a register it indexes
  // and adds back to (`v0 = &table; e = *(v0 + i * 4); p = v0 + e`).
  auto ConstAddressVars = [&](const HighFunc &Func) {
    VarKeyMap<unsigned> Assigned;
    VarKeyMap<va_t> Addresses;
    walkStmts(Func.Body, [&](const HighStmt &S) {
      if (S.Kind != StmtKind::Assign || !S.Dst || !S.Val ||
          (S.Dst->Kind != ExprKind::Var && S.Dst->Kind != ExprKind::Phi))
        return;
      const VarKey Key = varKey(S.Dst->Var);
      if (++Assigned[Key] == 1)
        if (const auto Address = constAddress(*S.Val))
          Addresses[Key] = *Address;
    });
    for (auto It = Addresses.begin(); It != Addresses.end();)
      It = Assigned[It->first] == 1 ? std::next(It) : Addresses.erase(It);
    return Addresses;
  };
  auto AddressOf = [&](const HighExpr &E,
                       const VarKeyMap<va_t> &Bases) -> std::optional<va_t> {
    if (const auto Address = constAddress(E))
      return Address;
    const HighExpr *Inner = peelIntegerViewOps(&E);
    if (Inner && (Inner->Kind == ExprKind::Var || Inner->Kind == ExprKind::Phi))
      if (const auto It = Bases.find(varKey(Inner->Var)); It != Bases.end())
        return It->second;
    return std::nullopt;
  };
  // The table an indexed address reads, its base a constant or such a
  // variable.
  auto IndexedTable = [&](const HighExpr &Address,
                          const VarKeyMap<va_t> &Bases) -> std::optional<va_t> {
    if (const HighExpr *Entries = indexedImageBase(Address))
      return Entries->ConstVal;
    const HighExpr *Sum = peelIntegerViewOps(&Address);
    if (!Sum || Sum->Kind != ExprKind::BinOp || Sum->Op != NdOp::INT_ADD ||
        Sum->Operands.size() != 2 || !Sum->Operands[0] || !Sum->Operands[1])
      return std::nullopt;
    for (unsigned I = 0; I < 2; ++I)
      if (const HighExpr *Base = peelIntegerViewOps(Sum->Operands[I].get());
          Base && (Base->Kind == ExprKind::Var || Base->Kind == ExprKind::Phi))
        if (const auto It = Bases.find(varKey(Base->Var));
            It != Bases.end() && isImageDataAddress(It->second) &&
            !constAddress(*Sum->Operands[1 - I]))
          return It->second;
    return std::nullopt;
  };
  VarKeyMap<std::pair<va_t, uint16_t>> TableEntryVars;
  // An entry added back to its table in an assignment proves the table
  // relative however the sum is used (`p = table + e; *p`).
  std::set<std::pair<va_t, uint16_t>> AddedBackTables;
  for (const HighFunc &Func : Funcs) {
    const VarKeyMap<va_t> Bases = ConstAddressVars(Func);
    walkStmts(Func.Body, [&](const HighStmt &S) {
      if (S.Kind != StmtKind::Assign || !S.Dst || !S.Val ||
          (S.Dst->Kind != ExprKind::Var && S.Dst->Kind != ExprKind::Phi))
        return;
      const HighExpr *Load = peelIntegerViewOps(S.Val.get());
      if (!Load || Load->Kind != ExprKind::Load || !Load->Type ||
          Load->Operands.empty() || !Load->Operands[0])
        return;
      if (const auto Table = IndexedTable(*Load->Operands[0], Bases))
        TableEntryVars[varKey(S.Dst->Var)] = {*Table, Load->Type->Size};
    });
    walkStmts(Func.Body, [&](const HighStmt &S) {
      if (S.Kind != StmtKind::Assign || !S.Val)
        return;
      const HighExpr *Sum = peelIntegerViewOps(S.Val.get());
      if (!Sum || Sum->Kind != ExprKind::BinOp || Sum->Op != NdOp::INT_ADD ||
          Sum->Operands.size() != 2 || !Sum->Operands[0] || !Sum->Operands[1])
        return;
      for (unsigned I = 0; I < 2; ++I) {
        const auto Table = AddressOf(*Sum->Operands[I], Bases);
        const HighExpr *Entry = peelIntegerViewOps(Sum->Operands[1 - I].get());
        if (!Table || !Entry ||
            (Entry->Kind != ExprKind::Var && Entry->Kind != ExprKind::Phi))
          continue;
        if (const auto It = TableEntryVars.find(varKey(Entry->Var));
            It != TableEntryVars.end() && It->second.first == *Table)
          AddedBackTables.insert(It->second);
      }
    });
  }
  // The constant addresses the code reads or writes at, as opposed to those
  // it uses as values.
  std::set<const HighExpr *> AccessAddresses;
  auto NoteAccessAddress = [&](const HighExpr *Address) {
    if (const HighExpr *Inner = unwrapIntegerView(Address);
        Inner && Inner->Kind == ExprKind::Const)
      AccessAddresses.insert(Inner);
  };
  // An atomic access needs its address as aligned in C as in the image.
  auto NoteAtomic = [&](const HighExpr &Address, const TypeRef &Access) {
    if (!Access)
      return;
    if (auto VA = constAddress(Address))
      if (auto It = ImageObjects.find(*VA); It != ImageObjects.end())
        It->second.AtomicBytes = std::max(It->second.AtomicBytes, Access->Size);
  };
  // An alignment precondition belongs to the access it guards: its address
  // is that access's, which needs the alignment in C as in the image.  The
  // guard comes before the access that declares the object, so the
  // alignment applies once the walk is done.
  std::map<va_t, uint16_t> GuardedAlignments;
  auto NoteAlignmentGuard = [&](const HighExpr &Call) {
    if (Call.Kind != ExprKind::Call ||
        Call.IntrinsicId != Intrinsic::RequireAligned ||
        Call.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        Call.Operands.size() < 2 || !Call.Operands[0] || !Call.Operands[1] ||
        Call.Operands[1]->Kind != ExprKind::Const)
      return;
    NoteAccessAddress(Call.Operands[0].get());
    const uint64_t Bytes = Call.Operands[1]->ConstVal;
    if (auto VA = constAddress(*Call.Operands[0]);
        VA && Bytes > 1 && Bytes <= UINT16_MAX)
      GuardedAlignments[*VA] = std::max<uint16_t>(GuardedAlignments[*VA],
                                                  static_cast<uint16_t>(Bytes));
  };
  std::function<void(const HighExpr &)> Visit = [&](const HighExpr &E) {
    if (E.Kind == ExprKind::Const && isAddressProvenance(E.ConstProvenance))
      noteFunctionAddress(functionAddressKey(E), Funcs);
    // An address anchored before its owner's section reaches the section's
    // first object, which the code indexes from there; what lies at the
    // address itself is no object the code uses.
    if (E.Kind == ExprKind::Const)
      if (const auto Owner = anchoredOwner(E)) {
        if (!stringLiteral(*Owner))
          NoteWhole(*Owner, nullptr, false);
        return;
      }
    // An address into an object, not at its start, that the code keeps as a
    // value is a pointer it walks over the object (`p = &sig[2]; p++`).
    if (E.Kind == ExprKind::Const && !AccessAddresses.count(&E) &&
        (E.ConstProvenance == ConstantAddressProvenance::Address ||
         E.ConstProvenance == ConstantAddressProvenance::DataAddress))
      if (const auto Object = sizedObjectAt(E.ConstVal);
          Object && Object->first != E.ConstVal)
        NoteWhole(E.ConstVal, nullptr, false);
    if (E.Kind == ExprKind::Const && isImageDataAddress(E.ConstVal) &&
        !stringLiteral(E.ConstVal)) {
      bool Named = Opts.UserNames && Opts.UserNames->count(E.ConstVal);
      if (!Named && Dbg) {
        if (auto Data = Dbg->resolveDataObject(E.ConstVal);
            Data && !Data->Name.empty() && !isStringLiteralName(Data->Name))
          Named = true;
      }
      if (!Named && Opts.Image) {
        if (const Symbol *Sym = Opts.Image->findSymbolAt(E.ConstVal);
            Sym && !Sym->IsFunc && !Sym->Name.empty() &&
            llvm::StringRef(Sym->Name).find(kAutoFuncPrefix) != 0)
          Named = true;
      }
      // Empty/non-ASCII rdata stays a named object (`&pwstr`), not a hex
      // immediate. Printable C/wchar literals still fold at the call site.
      if (Named) {
        noteImageAddress(E.ConstVal);
        if (!AccessAddresses.count(&E))
          ImageObjects[E.ConstVal].AddressTaken = true;
      } else if (!AccessAddresses.count(&E) && Opts.Image &&
                 Opts.Image->isELF() && Opts.Image->IsRelocatable &&
                 (E.ConstProvenance == ConstantAddressProvenance::Address ||
                  E.ConstProvenance ==
                      ConstantAddressProvenance::DataAddress)) {
        // An unnamed object whose start the code keeps as a value, at an
        // address its relocation proves, is declared whole: the initializer
        // of a local table that -O0 code copies with memcpy.  Only the
        // sections of an ELF object hold one translation unit's data.  The
        // unnamed bytes around such an address in a linked image are a whole
        // section of other objects, and a Mach-O object's metadata sections
        // point at symbols the output does not define.
        NoteWhole(E.ConstVal, nullptr, false);
      } else if (Opts.AccessWidths && !AccessAddresses.count(&E) &&
                 (E.ConstProvenance == ConstantAddressProvenance::Address ||
                  E.ConstProvenance ==
                      ConstantAddressProvenance::DataAddress)) {
        // Unnamed data whose address the code proves and keeps as a value
        // takes the name the listing gives it (`&qword_140C4EE80`), not a
        // number, when the emitter knows how the listing names data.  An
        // address inside a sized object reaches that object, declared whole
        // above.
        const auto Object = sizedObjectAt(E.ConstVal);
        if (!Object || Object->first == E.ConstVal) {
          noteImageAddress(E.ConstVal);
          ImageObjects[E.ConstVal].AddressTaken = true;
        }
      }
    }
    // A data import read names the import's slot; the read-only pointer it
    // may be read through (MinGW's `.refptr`) is no object of the program's.
    if (E.Kind == ExprKind::Load &&
        E.MemoryOrdering == NdMemoryOrdering::None &&
        E.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
        !E.Operands.empty() && E.Operands[0])
      if (const auto Slot =
              importDataSlotRead(*E.Operands[0], E.Type ? E.Type->Size : 0)) {
        ImportDataSlotReads.insert(*Slot);
        return;
      }
    if (E.Kind == ExprKind::Load &&
        E.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
        E.MemoryOrdering == NdMemoryOrdering::None && E.Type &&
        E.Type->Size == Opts.Image->getPointerSize() && !E.Operands.empty() &&
        E.Operands[0])
      if (const auto Slot = constAddress(*E.Operands[0]))
        if (const auto It = DataSymbolBindings.find(*Slot);
            It != DataSymbolBindings.end() && It->second.Immutable) {
          UsedDataBindings.insert(*Slot);
          if (It->second.Definition) {
            NoteWhole(*It->second.Definition, nullptr, false);
            noteImageAddress(*It->second.Definition);
          }
          return;
        }
    if (E.Kind == ExprKind::Load &&
        E.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
        !E.Operands.empty() && E.Operands[0]) {
      NoteAccessAddress(E.Operands[0].get());
      if (auto VA = constAddress(*E.Operands[0])) {
        const uint16_t Size = E.Type ? E.Type->Size : 0;
        if (!foldReadonlyScalar(*VA, Size))
          noteImageObject(*VA, E.Type, false, true);
      }
      if (E.MemoryOrdering != NdMemoryOrdering::None)
        NoteAtomic(*E.Operands[0], E.Type);
    }
    // A table read or written at a variable offset is the whole table.
    if ((E.Kind == ExprKind::Load || E.Kind == ExprKind::Store) &&
        E.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
        !E.Operands.empty() && E.Operands[0])
      if (const HighExpr *Base = indexedImageBase(*E.Operands[0])) {
        const TypeRef Access =
            E.Kind == ExprKind::Load
                ? E.Type
                : (E.Operands.size() > 1 && E.Operands[1] ? E.Operands[1]->Type
                                                          : nullptr);
        NoteWhole(Base->ConstVal, Access, E.Kind == ExprKind::Store,
                  E.Operands[0].get());
        // `*(T *)(table + table[i])`: the offset the code adds to the table
        // is an entry of the same table.
        std::function<void(const HighExpr &, unsigned)> Terms =
            [&](const HighExpr &Term, unsigned Depth) {
              const HighExpr *Inner = peelIntegerViewOps(&Term);
              if (!Inner || Depth > limits::kMaxIntegerViewUnwrapDepth)
                return;
              if (Inner->Kind == ExprKind::BinOp &&
                  Inner->Op == NdOp::INT_ADD && Inner->Operands.size() == 2) {
                for (const ExprPtr &Operand : Inner->Operands)
                  if (Operand)
                    Terms(*Operand, Depth + 1);
                return;
              }
              if (Inner->Kind == ExprKind::Var ||
                  Inner->Kind == ExprKind::Phi) {
                if (const auto Entry = TableEntryVars.find(varKey(Inner->Var));
                    Entry != TableEntryVars.end() &&
                    Entry->second.first == Base->ConstVal)
                  NoteRelativeTargets(Base->ConstVal, Entry->second.second,
                                      /*AddedBack=*/true);
                return;
              }
              if (Inner->Kind != ExprKind::Load || !Inner->Type ||
                  Inner->Operands.empty() || !Inner->Operands[0])
                return;
              if (const HighExpr *Entries =
                      indexedImageBase(*Inner->Operands[0]);
                  Entries && Entries->ConstVal == Base->ConstVal)
                NoteRelativeTargets(Base->ConstVal, Inner->Type->Size,
                                    /*AddedBack=*/true);
            };
        Terms(*E.Operands[0], 0);
      }
    if (E.Kind == ExprKind::Store &&
        E.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
        E.Operands.size() >= 2 && E.Operands[0]) {
      NoteAccessAddress(E.Operands[0].get());
      if (auto VA = constAddress(*E.Operands[0]))
        noteImageObject(*VA, E.Operands[1] ? E.Operands[1]->Type : nullptr,
                        true, true);
      if (E.MemoryOrdering != NdMemoryOrdering::None)
        NoteAtomic(*E.Operands[0],
                   E.Operands[1] ? E.Operands[1]->Type : nullptr);
    }
    // A floating-point state instruction's memory form reads its operand at
    // the address it takes, as a load of that width would: a constant-pool
    // double that only MULSD reads is that double, not a weak placeholder.
    if (E.Kind == ExprKind::Call &&
        isX86FPStateMemoryIntrinsic(E.IntrinsicId) &&
        E.MemoryAddressSpace == NdMemoryAddressSpace::Default && E.Type &&
        E.Operands.size() > 1 && E.Operands[0] && E.Operands[1] &&
        E.Operands[1]->Kind == ExprKind::Const)
      if (const unsigned Bytes = x86FPStateMemoryBytes(
              E.IntrinsicId, E.Type->Size,
              static_cast<unsigned>(E.Operands[1]->ConstVal))) {
        NoteAccessAddress(E.Operands[0].get());
        if (auto VA = constAddress(*E.Operands[0]))
          noteImageObject(*VA, NdType::makeInt(Bytes, false), false, true);
        // Through a base the code adds (i386 PIC code adds the GOT register
        // to the constant's offset), the read is of the object there, as a
        // load at a variable offset is.
        else if (const HighExpr *Base = indexedImageBase(*E.Operands[0]))
          NoteWhole(Base->ConstVal, NdType::makeInt(Bytes, false), false);
      }
    // A read-modify-write reads and writes the object it addresses.
    if (E.Kind == ExprKind::BinOp &&
        (E.Op == NdOp::ATOMIC_ADD || E.Op == NdOp::ATOMIC_XCHG ||
         E.Op == NdOp::ATOMIC_CMPXCHG) &&
        E.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
        !E.Operands.empty() && E.Operands[0]) {
      NoteAccessAddress(E.Operands[0].get());
      if (auto VA = constAddress(*E.Operands[0]))
        noteImageObject(*VA, E.Type, true, true);
      NoteAtomic(*E.Operands[0], E.Type);
    }
    if (E.Kind == ExprKind::Addr && !E.Operands.empty() && E.Operands[0] &&
        E.Operands[0]->Kind == ExprKind::Load &&
        E.Operands[0]->MemoryAddressSpace == NdMemoryAddressSpace::Default &&
        !E.Operands[0]->Operands.empty() && E.Operands[0]->Operands[0]) {
      if (auto VA = constAddress(*E.Operands[0]->Operands[0])) {
        noteImageObject(*VA, E.Operands[0]->Type, false);
        ImageObjects[*VA].AddressTaken = true;
      }
    }
    NoteAlignmentGuard(E);
    // An indirect callee is no printed operand, but the slot it loads is
    // image data all the same, named as the slot the code calls through.
    E.forEachChildExpr([&](const ExprPtr &Op) { Visit(*Op); });
    if (E.Kind != ExprKind::Call)
      return;
    if (E.IndirectTarget)
      if (auto VA = imageLoadVA(*E.IndirectTarget))
        if (auto It = ImageObjects.find(*VA); It != ImageObjects.end())
          It->second.CallSlot = true;
    const auto Callee = debugCallee(E);
    if (!Callee)
      return;
    for (size_t I = 0; I < E.Operands.size(); ++I) {
      if (!E.Operands[I])
        continue;
      const TypeRef Expected = expectedDebugCallArgType(*Callee, I);
      if (!Expected || Expected->Kind != NdTypeKind::Ptr ||
          !Expected->Pointee || Expected->Pointee->SourceName.empty())
        continue;
      // The object holds the pointer only where the code reads it whole: a
      // narrower read widened for the parameter (make's 4-byte output_sync)
      // is an integer of its own width.
      if (const auto Load = imageLoad(*E.Operands[I]);
          Load && Load->Bytes == Expected->Size)
        noteImageObject(Load->Addr, Expected, false);
    }
  };
  for (const HighFunc &Func : Funcs) {
    // A function that returns no value prints its returns bare (a returned
    // call as its statement): what the return register holds there names
    // nothing in C, such as the GOT address a get-PC sequence left in it.
    const TypeRef DeclaredReturn = declaredFunctionReturnType(Func);
    const bool BareReturns = DeclaredReturn
                                 ? DeclaredReturn->Kind == NdTypeKind::Void
                                 : analyzeVoidReturn(Func);
    Reading = &Func;
    walkStmts(Func.Body, [&](const HighStmt &S) {
      ReadingStmt = &S;
      if (BareReturns && S.Kind == StmtKind::Return && S.RetVal &&
          !highCExpressionHasEffect(*S.RetVal))
        return;
      if (S.Kind == StmtKind::Store &&
          S.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
          S.StoreAddr) {
        NoteAccessAddress(S.StoreAddr.get());
        if (auto VA = constAddress(*S.StoreAddr))
          noteImageObject(*VA, S.StoreVal ? S.StoreVal->Type : nullptr, true,
                          true);
        if (S.MemoryOrdering != NdMemoryOrdering::None)
          NoteAtomic(*S.StoreAddr, S.StoreVal ? S.StoreVal->Type : nullptr);
        // A table written at a variable offset is the whole table too.
        if (const HighExpr *Base = indexedImageBase(*S.StoreAddr))
          NoteWhole(Base->ConstVal, S.StoreVal ? S.StoreVal->Type : nullptr,
                    true, S.StoreAddr.get());
      }
      forEachExpr(S, [&](const ExprPtr &E) {
        if (E)
          Visit(*E);
      });
    });
  }
  Reading = nullptr;
  ReadingStmt = nullptr;
  ReadDefinitions();
  // A stretch the code reaches whole is declared whole, and so is one that
  // holds an address the code keeps as a value: through it the code may
  // reach any byte of the stretch.  Otherwise the bounded accesses declare
  // the bytes they reach of each table.
  if (!BoundedAccesses.empty()) {
    std::vector<va_t> Values;
    std::function<void(const HighExpr &, bool)> NoteValues =
        [&](const HighExpr &E, bool Addressing) {
          if (E.Kind == ExprKind::Const) {
            if (!Addressing)
              Values.push_back(E.ConstVal);
            return;
          }
          const auto Each = [&](bool Addresses) {
            E.forEachChildExpr([&](const ExprPtr &Child) {
              if (Child)
                NoteValues(*Child, Addresses);
            });
          };
          // What a load or store reads or writes at, as the sum of a base
          // and offsets in any view, is an address it uses, not a value; the
          // address of an element it would read is a value.
          switch (E.Kind) {
          case ExprKind::Load:
          case ExprKind::Store:
            for (size_t I = 0; I < E.Operands.size(); ++I)
              if (E.Operands[I])
                NoteValues(*E.Operands[I], I == 0);
            return;
          case ExprKind::Addr:
            if (!E.Operands.empty() && E.Operands[0] &&
                E.Operands[0]->Kind == ExprKind::Load &&
                !E.Operands[0]->Operands.empty() &&
                E.Operands[0]->Operands[0]) {
              NoteValues(*E.Operands[0]->Operands[0], false);
              return;
            }
            Each(false);
            return;
          case ExprKind::BinOp:
            Each(Addressing && E.Op == NdOp::INT_ADD);
            return;
          case ExprKind::Cast:
          case ExprKind::BitCast:
            Each(Addressing);
            return;
          case ExprKind::UnaryOp:
            Each(Addressing &&
                 (E.Op == NdOp::INT_ZEXT || E.Op == NdOp::INT_SEXT));
            return;
          // An alignment guard takes the address of the access it guards.
          case ExprKind::Call:
            if (E.IntrinsicId == Intrinsic::RequireAligned) {
              for (size_t I = 0; I < E.Operands.size(); ++I)
                if (E.Operands[I])
                  NoteValues(*E.Operands[I], I == 0);
              return;
            }
            Each(false);
            return;
          default:
            Each(false);
            return;
          }
        };
    for (const HighFunc &Func : Funcs)
      walkStmts(Func.Body, [&](const HighStmt &S) {
        forEachExpr(S, [&](const ExprPtr &E) {
          if (E)
            NoteValues(*E, S.Kind == StmtKind::Store && &E == &S.StoreAddr);
        });
      });
    llvm::sort(Values);
    for (const BoundedAccess &Bounded : BoundedAccesses) {
      const auto [Begin, Bytes] = Bounded.Stretch;
      const auto Kept = llvm::lower_bound(Values, Begin);
      const bool Whole = WholeStretches.count(Begin) ||
                         (Kept != Values.end() && *Kept - Begin < Bytes);
      const va_t At = Whole ? Begin : Bounded.Table;
      noteImageObject(At, Bounded.Access, Bounded.Written,
                      Bounded.Access != nullptr);
      ImageObject &Obj = ImageObjects[At];
      Obj.IndexedBytes =
          std::max(Obj.IndexedBytes, Whole ? Bytes : Bounded.Reach);
    }
  }
  for (const auto &[VA, Bytes] : GuardedAlignments) {
    // A string an aligned access reads where it starts is the object C
    // aligns as the image does (`_Alignas`), declared as its array below, not
    // a literal C places where it likes.
    if (!ImageObjects.count(VA) && stringLiteral(VA))
      noteImageAddress(VA);
    if (auto It = ImageObjects.find(VA); It != ImageObjects.end())
      It->second.AlignedBytes = std::max(It->second.AlignedBytes, Bytes);
  }

  // A table the code keeps only the address of, through a variable it
  // indexes, may be relative all the same: every entry an offset to a sized
  // object after it.
  for (const auto &[Table, EntryBytes] : AddedBackTables)
    NoteRelativeTargets(Table, EntryBytes, /*AddedBack=*/true);
  for (auto &[Addr, Obj] : ImageObjects)
    if (Obj.AddressTaken)
      NoteRelativeTargets(Addr, 4, /*AddedBack=*/false);

  // A pointer slot the image relocates holds an address, not a number: a
  // table of functions or of data the code reaches through it.  Each target
  // is declared, data whole, so the slot can name it; a string stays its
  // literal.  A target table may hold slots of its own.
  const unsigned PointerBytes = pointerBytes(Opts.TheArch);
  // Those slots and the ones a data symbol binds, in address order.  The
  // image's sets are read where they are: it relocates tens of thousands.
  std::set<va_t> BoundSlots;
  for (const auto &[Slot, Binding] : DataSymbolBindings) {
    (void)Binding;
    BoundSlots.insert(Slot);
  }
  const std::set<va_t> *const SlotSets[] = {&Opts.Image->CodePtrRelocSlots,
                                            &Opts.Image->DataPtrRelocSlots,
                                            &BoundSlots};
  // The first of them at or after Addr.
  auto NextSlot = [&](va_t Addr) -> std::optional<va_t> {
    std::optional<va_t> Next;
    for (const std::set<va_t> *Slots : SlotSets)
      if (const auto It = Slots->lower_bound(Addr);
          It != Slots->end() && (!Next || *It < *Next))
        Next = *It;
    return Next;
  };
  auto SlotAfter = [&](va_t Slot) -> std::optional<va_t> {
    if (Slot == std::numeric_limits<va_t>::max())
      return std::nullopt;
    return NextSlot(Slot + 1);
  };
  // An object the code keeps the address of reaches each of its slots
  // through that address, whichever the code indexes (`T = c ? FA : FB;
  // T[i]` indexes neither table directly): its symbol sizes it.
  auto Extent = [&](va_t Addr, const ImageObject &Obj) -> uint64_t {
    uint64_t Bytes = std::max<uint64_t>(
        Obj.IndexedBytes,
        Obj.String ? Obj.ArrayBytes : (Obj.Type ? Obj.Type->Size : 0));
    if (Obj.AddressTaken)
      if (const auto Sized = sizedObjectAt(Addr); Sized && Sized->first == Addr)
        Bytes = std::max<uint64_t>(Bytes, Sized->second);
    return Bytes;
  };
  std::set<va_t> NotedSlots;
  for (bool Changed = NextSlot(0).has_value(); Changed;) {
    Changed = false;
    for (auto &[Addr, Obj] : ImageObjects) {
      const uint64_t Size = Extent(Addr, Obj);
      for (auto Slot = NextSlot(Addr);
           Slot && *Slot - Addr < Size && *Slot - Addr + PointerBytes <= Size;
           Slot = SlotAfter(*Slot)) {
        if (!NotedSlots.insert(*Slot).second)
          continue;
        Changed = true;
        if (const auto Binding = DataSymbolBindings.find(*Slot);
            Binding != DataSymbolBindings.end()) {
          UsedDataBindings.insert(*Slot);
          Obj.HoldsPointers = true;
          if (Binding->second.Definition) {
            NoteWhole(*Binding->second.Definition, nullptr, false);
            noteImageAddress(*Binding->second.Definition);
          }
          continue;
        }
        const uint8_t *Bytes = Opts.Image->readVA(*Slot, PointerBytes);
        if (!Bytes)
          continue;
        const va_t Target = PointerBytes == 8 ? readLE<uint64_t>(Bytes)
                                              : readLE<uint32_t>(Bytes);
        if (Opts.Image->CodePtrRelocSlots.count(*Slot)) {
          noteFunctionAddress(Target, Funcs);
          noteFunctionAddress(Target & ~va_t{1}, Funcs);
        } else if (sizedObjectAt(Target)) {
          NoteWhole(Target, nullptr, false);
        }
        // Only a slot whose target C names gains from holding its address.
        Obj.HoldsPointers |=
            Opts.Image->CodePtrRelocSlots.count(*Slot)
                ? FunctionAddressNames.count(Target) ||
                      FunctionAddressNames.count(Target & ~va_t{1})
                : sizedObjectAt(Target) || ImageObjects.count(Target) ||
                      stringLiteral(Target).has_value();
      }
    }
  }

  // A string the code reaches by its address is declared as its array, and a
  // pointer slot the code only loads, holding the address of a read-only
  // string, as that string's pointer.
  for (auto &[Addr, Obj] : ImageObjects) {
    if (Obj.Written || (Obj.Type && Obj.Type->Kind != NdTypeKind::Int &&
                        Obj.Type->Kind != NdTypeKind::Ptr))
      continue;
    // Without includes nothing declares char16_t and char32_t.
    auto Declarable = [&](const std::optional<ImageCString> &String) {
      return String && (String->UnitBytes == 1 || Opts.EmitIncludes);
    };
    if (Obj.MemoryWidths.empty()) {
      auto String = imageCString(Opts.Image, Addr);
      if (!Declarable(String))
        continue;
      // The object's own extent holds the string; bytes after it are zero,
      // as the array's initializer leaves them.
      uint64_t Bytes = String->Bytes;
      if (const uint64_t Declared = Opts.Image->dataObjectSizeAt(Addr)) {
        if (Declared < Bytes || Declared % String->UnitBytes)
          continue;
        const uint8_t *Tail =
            Opts.Image->readVA(Addr + Bytes, Declared - Bytes);
        if (Declared > Bytes &&
            (!Tail || !std::all_of(Tail, Tail + (Declared - Bytes),
                                   [](uint8_t Byte) { return Byte == 0; })))
          continue;
        Bytes = Declared;
      }
      Obj.String = std::move(String);
      Obj.ArrayBytes = Bytes;
    } else if (Obj.MemoryWidths.size() == 1 &&
               *Obj.MemoryWidths.begin() == PointerBytes &&
               (!Obj.Type || Obj.Type->Kind == NdTypeKind::Int ||
                !Obj.Type->Pointee || Obj.Type->Pointee->SourceName.empty())) {
      const uint8_t *Slot = Opts.Image->readVA(Addr, PointerBytes);
      if (!Slot)
        continue;
      const va_t Target =
          PointerBytes == 8 ? readLE<uint64_t>(Slot) : readLE<uint32_t>(Slot);
      const Segment *Seg = Opts.Image->getSegmentFor(Target);
      if (!Seg || Seg->isWritable())
        continue;
      if (auto String = imageCString(Opts.Image, Target); Declarable(String))
        Obj.PointsTo = std::move(String);
    }
  }

  // An object whose address the code uses as a value is reached whole
  // through it, unless its string or pointer declaration holds it already:
  // the integer that stands in for it holds at most eight of its bytes.
  for (auto &[Addr, Obj] : ImageObjects)
    if (Obj.AddressTaken && !Obj.String && !Obj.PointsTo && Obj.Type)
      if (const auto Object = sizedObjectAt(Addr);
          Object && Object->first == Addr && Object->second > Obj.Type->Size)
        Obj.IndexedBytes = std::max(Obj.IndexedBytes, Object->second);

  // A C _BitInt(80) object can occupy 16 bytes while the guest x87 value
  // occupies 10. Also, independently declared globals cannot represent two
  // image accesses whose guest address ranges overlap. Project each connected
  // alias range as one byte array and use exact-width memory helpers at uses.
  va_t GroupBase = 0, GroupEnd = 0;
  unsigned GroupCount = 0;
  bool NeedsBacking = false;
  // A backing that starts at a named object takes its name; the object is
  // only ever printed through the backing.  One that holds word-aligned
  // relocated pointer slots is words, so each slot can hold an address.
  auto FinishGroup = [&] {
    if (!GroupCount || !NeedsBacking)
      return;
    const auto First = ImageObjects.find(GroupBase);
    ImageBacking Backing{
        GroupBase,
        GroupEnd,
        First != ImageObjects.end() && !First->second.Name.empty()
            ? First->second.Name
            : GlobalIdentifierAllocator.allocate(
                  makeSyntheticGlobalName(GroupBase) + "_bytes", "g"),
        {},
        {}};
    for (auto Slot = NextSlot(GroupBase); Slot && *Slot < GroupEnd;
         Slot = SlotAfter(*Slot)) {
      if ((*Slot - GroupBase) % PointerBytes ||
          *Slot + PointerBytes > GroupEnd) {
        Backing.PointerSlots.clear();
        break;
      }
      Backing.PointerSlots.push_back(*Slot);
    }
    if (!Backing.PointerSlots.empty()) {
      Backing.Words = Backing.Name;
      Backing.Name += ".b";
    }
    // An atomic access in the bytes, or one that requires its alignment,
    // needs its address as aligned in C as in the image: bytes before the
    // group give every address its residue modulo the widest one.  Words
    // take them as whole words, which keeps each slot word-aligned: a group
    // that starts at a word does (sort's table, 8 bytes past a 16-byte
    // boundary, which a 16-byte access in it requires).
    for (auto It = ImageObjects.lower_bound(GroupBase);
         It != ImageObjects.end() && It->first < GroupEnd; ++It) {
      const ImageObject &Obj = It->second;
      const unsigned Align = static_cast<unsigned>(std::max<uint64_t>(
          Obj.AtomicBytes
              ? std::min<uint64_t>(llvm::PowerOf2Ceil(Obj.AtomicBytes),
                                   kMaxAtomicAccessBytes)
              : 0,
          Obj.AlignedBytes));
      if (Align <= 1)
        continue;
      if (!Backing.Words.empty() && (GroupBase % Align) % PointerBytes)
        throw std::invalid_argument(
            std::string(Obj.AtomicBytes ? "an atomic" : "an aligned") +
            " access in relocated image words at 0x" +
            llvm::utohexstr(It->first) +
            " is less aligned in C than in the image");
      Backing.Align = std::max(Backing.Align, Align);
    }
    Backing.Pad = GroupBase % Backing.Align;
    // A C library header's object is the header's, and the reads of its
    // members within it: its bytes are its address's, as aligned as the
    // header declares it.
    const auto WithinHeaderObject = [&] {
      if (First == ImageObjects.end() || First->first != GroupBase ||
          First->second.Header.empty())
        return false;
      const uint64_t Own = Opts.Image->dataObjectSizeAt(GroupBase);
      return GroupCount == 1 || (Own && GroupEnd - GroupBase <= Own);
    };
    if (WithinHeaderObject() && Backing.Words.empty()) {
      Backing.Name = "((unsigned char *)&" + First->second.Name + ")";
      Backing.Header = true;
      Backing.Align = 1;
      Backing.Pad = 0;
    }
    ImageBackings.push_back(std::move(Backing));
  };
  for (const auto &[Addr, Obj] : ImageObjects) {
    const uint64_t Size = std::max<uint64_t>(
        Obj.IndexedBytes,
        Obj.String ? Obj.ArrayBytes : (Obj.Type ? Obj.Type->Size : 0));
    if (!Size || Addr > std::numeric_limits<va_t>::max() - Size)
      llvm::report_fatal_error("HighC image object has invalid extent");
    const va_t End = Addr + Size;
    if (GroupCount && Addr >= GroupEnd) {
      FinishGroup();
      GroupCount = 0;
      GroupEnd = 0;
      NeedsBacking = false;
    }
    if (!GroupCount)
      GroupBase = Addr;
    else
      NeedsBacking = true;
    GroupEnd = std::max(GroupEnd, End);
    ++GroupCount;
    // An object less aligned in the image than an access there requires
    // keeps that residue in bytes padded to the alignment: the access
    // faults in C as in the image.
    NeedsBacking |=
        Obj.MemoryWidths.size() > 1 || Obj.IndexedBytes || Obj.HeaderBytes ||
        (Obj.HoldsPointers && !Obj.PointsTo && Size > PointerBytes) ||
        (Obj.AlignedBytes > 1 && Addr % Obj.AlignedBytes);
    for (uint16_t Width : Obj.MemoryWidths)
      NeedsBacking |= Width && (Width & (Width - 1)) != 0;
  }
  FinishGroup();
}

void HighCWriter::noteFunctionAddress(va_t Addr,
                                      const std::vector<HighFunc> &Funcs) {
  if (FunctionAddressNames.count(Addr))
    return;
  for (const HighFunc &Func : Funcs)
    if (Func.Entry == Addr && !Func.Name.empty()) {
      FunctionAddressNames.emplace(Addr, functionIdentifier(Func));
      AddressTakenDefinitions.insert(&Func);
      return;
    }
  // A function the image names; this output declares it.
  if (!SharedFunctionSymbols && !OwnFunctionSymbols)
    OwnFunctionSymbols = collectImageFunctionSymbols(*Opts.Image);
  const HighCImageFunctionSymbols &Symbols =
      SharedFunctionSymbols ? *SharedFunctionSymbols : *OwnFunctionSymbols;
  if (const auto Found = Symbols.find(Addr); Found != Symbols.end()) {
    const Symbol &Sym = *Found->second;
    FunctionAddressNames.emplace(Addr,
                                 functionIdentifier(llvm::StringRef(Sym.Name)));
    AddressTakenFunctions.insert(Sym.Name);
  }
}

void HighCWriter::writeImageObjects() {
  for (va_t Slot : UsedDataBindings) {
    const auto &Binding = DataSymbolBindings.at(Slot);
    if (Binding.Definition || ExternalDataNames.count(Binding.Name))
      continue;
    const auto Identifier = GlobalIdentifierAllocator.allocate(
        "neverd_data_" + Binding.Name, "neverd_data");
    ExternalDataNames.emplace(Binding.Name, Identifier);
    // An untyped byte alias binds the linker's identity without inventing a
    // source type or colliding with declarations such as stdio's stdout.
    OS << "extern unsigned char " << Identifier << "[] __asm__(\"";
    OS.write_escaped(Binding.Name);
    OS << "\")";
    if (Binding.Weak)
      OS << " __attribute__((weak))";
    OS << ";\n";
  }
  if (ImageObjects.empty())
    return;
  for (const ImageBacking &Backing : ImageBackings) {
    if (!Backing.Words.empty() || Backing.Header)
      continue;
    if (Opts.EmitComments)
      OS << "/* neverd.image: 0x" << llvm::utohexstr(Backing.Base) << " .. 0x"
         << llvm::utohexstr(Backing.End) << backingReadable(Backing) << " */\n";
    if (Backing.Align > 1)
      OS << "_Alignas(" << Backing.Align << ") ";
    const bool Authenticated = Opts.Image->hasMachOAuthenticatedPointerIn(
        Backing.Base, Backing.End - Backing.Base);
    if (Authenticated)
      OS << "extern ";
    OS << "unsigned char " << Backing.Name << "["
       << Backing.Pad + (Backing.End - Backing.Base) << "]";
    if (!Authenticated)
      writeBackingBytes(Backing);
    OS << ";\n";
  }
  // Every object has its name before any initializer names it.
  for (auto &[Addr, Obj] : ImageObjects) {
    if (imageBackingAddress(Addr) || !Obj.Name.empty())
      continue;
    // As the listing names it: a slot the loader relocates a pointer into,
    // or one the code only calls through; the code's reads give a slot it
    // loads a callee from their width.
    const bool PointerSlot =
        Opts.Image->CodePtrRelocSlots.count(Addr) ||
        Opts.Image->DataPtrRelocSlots.count(Addr) ||
        (Obj.CallSlot &&
         !(Opts.AccessWidths && Opts.AccessWidths->at(Addr).has_value()));
    const auto Binding = DataSymbolBindings.find(Addr);
    Obj.Name = GlobalIdentifierAllocator.allocate(
        Binding != DataSymbolBindings.end()
            ? "got_" + Binding->second.Name
            : makeDataName(Addr, PointerSlot, dataNameWidth(Addr, Obj)),
        "g");
  }
  // A pointer slot that names another object comes after the declarations
  // of everything it can name.
  std::vector<va_t> Deferred;
  for (auto &[Addr, Obj] : ImageObjects) {
    // An object a C library header declares is the header's.
    if (imageBackingAddress(Addr) || !Obj.Header.empty())
      continue;
    const uint64_t ObjectBytes = std::max<uint64_t>(
        Obj.IndexedBytes,
        Obj.Type ? Obj.Type->Size : pointerBytes(Opts.TheArch));
    if (Opts.Image->hasMachOAuthenticatedPointerIn(Addr, ObjectBytes)) {
      OS << "extern " << declarationToC(Obj.Type, Obj.Name) << ";"
         << kArm64eAuthenticatedStorageComment << "\n";
      continue;
    }
    if (!Obj.String && !Obj.PointsTo && relocatedSlotInitializer(Addr, Obj)) {
      Deferred.push_back(Addr);
      continue;
    }
    if (Opts.EmitComments)
      OS << "/* neverd.image: 0x" << llvm::utohexstr(Addr)
         << (Obj.Readable.empty() ? "" : " ") << Obj.Readable << " */\n";
    // A definition so standalone HighC can link, with the value the image
    // holds.  LLVMC keeps `extern` because it projects LLVM `external global`.
    std::string Note;
    // As aligned as an access at its address requires, which the image's
    // address is: a less aligned one is bytes (above).
    if (Obj.AlignedBytes > 1)
      OS << "_Alignas(" << Obj.AlignedBytes << ") ";
    if (Obj.String) {
      const Segment *Seg = Opts.Image->getSegmentFor(Addr);
      OS << (Seg && !Seg->isWritable() ? "const " : "") << Obj.String->Element
         << " " << Obj.Name << "[";
      // The literal sizes an array that ends with the string.
      if (Obj.ArrayBytes != Obj.String->Bytes)
        OS << Obj.ArrayBytes / Obj.String->UnitBytes;
      OS << "] = " << Obj.String->Literal;
      Note = Obj.String->Note;
    } else if (Obj.PointsTo) {
      OS << "const " << Obj.PointsTo->Element << " *" << Obj.Name << " = "
         << Obj.PointsTo->Literal;
      Note = Obj.PointsTo->Note;
    } else {
      OS << declarationToC(Obj.Type, Obj.Name);
      if (auto Value = imageObjectInitializer(Addr, Obj)) {
        OS << " = " << *Value;
        if (Obj.Type && Obj.Type->Size == pointerBytes(Opts.TheArch))
          Note = relocatedTargetNote(Addr);
      }
    }
    if (Note.empty() && !Obj.String && !Obj.PointsTo && Opts.EmitComments)
      if (const auto Text = imageStringComment(Opts.Image, Addr))
        Note = *Text;
    OS << ";";
    if (Opts.EmitComments && !Note.empty())
      OS << " /* " << Note << " */";
    OS << "\n";
  }
  writePointerBackings(Deferred);
  OS << "\n";
}

void HighCWriter::writeBackingBytes(const ImageBacking &Backing) {
  // The bytes after the alignment padding are the image's; a byte it does
  // not hold is zero.
  std::vector<uint8_t> Bytes(Backing.Pad + (Backing.End - Backing.Base));
  for (va_t Addr = Backing.Base; Addr < Backing.End; ++Addr)
    if (const uint8_t *Byte = Opts.Image->readVA(Addr, 1))
      Bytes[Backing.Pad + (Addr - Backing.Base)] = *Byte;
  // C zeroes what an initializer leaves out, so it stops at the last nonzero
  // byte.  A list gives the bytes in order and names an index only after
  // more zeros than it takes to write (" 0," each).
  size_t Used = Bytes.size();
  while (Used && !Bytes[Used - 1])
    --Used;
  if (!Used)
    return;
  std::vector<std::string> Elements;
  size_t ListChars = 0;
  for (size_t I = 0, Next = 0; I < Used; ++I) {
    if (!Bytes[I])
      continue;
    std::string Element = "[" + std::to_string(I) + "] = ";
    if ((I - Next) * 3 <= Element.size()) {
      Elements.insert(Elements.end(), I - Next, "0");
      Element.clear();
    }
    Elements.push_back(Element + byteListElement(Bytes[I]));
    Next = I + 1;
  }
  for (const std::string &Element : Elements)
    ListChars += Element.size() + 2;
  // A string is shorter for text, and reads as it.  C promises compilers
  // take strings of 4095 characters, and stores a NUL after them, so the
  // array needs a byte to spare (C++ and GCC's
  // -Wunterminated-string-initialization require it).
  if (Used <= ::neverd::limits::backend::kHighCEmitterImageStringMaxBytes &&
      Used < Bytes.size()) {
    std::string Escaped;
    std::vector<size_t> Ends(Used);
    for (size_t I = 0; I < Used; ++I) {
      Escaped += escapeCString(
          llvm::StringRef(reinterpret_cast<const char *>(&Bytes[I]), 1));
      Ends[I] = Escaped.size();
    }
    if (Escaped.size() < ListChars) {
      // A line ends between bytes: after a NUL or a newline once it is long
      // enough, where the image's strings end, and before it runs too long.
      std::vector<std::pair<size_t, size_t>> Lines;
      size_t LineBegin = 0;
      for (size_t I = 0; I < Used; ++I) {
        const size_t PieceBegin = I ? Ends[I - 1] : 0;
        if (PieceBegin > LineBegin && Ends[I] - LineBegin > ImageLineChars) {
          Lines.emplace_back(LineBegin, PieceBegin);
          LineBegin = PieceBegin;
        }
        if ((!Bytes[I] || Bytes[I] == '\n') &&
            Ends[I] - LineBegin >= ImageStringBreakChars) {
          Lines.emplace_back(LineBegin, Ends[I]);
          LineBegin = Ends[I];
        }
      }
      if (LineBegin < Escaped.size())
        Lines.emplace_back(LineBegin, Escaped.size());
      if (Lines.size() == 1) {
        OS << " = \"" << Escaped << "\"";
        return;
      }
      OS << " =";
      for (const auto &[Begin, End] : Lines)
        OS << "\n  \"" << llvm::StringRef(Escaped).slice(Begin, End) << "\"";
      return;
    }
  }
  // A long list takes a few elements a line, so no line runs to the
  // object's size.
  const bool Wrapped = ListChars > ImageLineChars;
  OS << " = {";
  size_t Line = 0;
  for (const std::string &Element : Elements) {
    if (Wrapped && (!Line || Line + Element.size() + 2 > ImageLineChars)) {
      OS << "\n ";
      Line = 0;
    }
    OS << " " << Element << ",";
    Line += Element.size() + 2;
  }
  OS << (Wrapped ? "\n}" : " }");
}

std::string HighCWriter::backingReadable(const ImageBacking &Backing) const {
  const auto First = ImageObjects.find(Backing.Base);
  if (First == ImageObjects.end() || First->second.Readable.empty() ||
      First->second.Name !=
          (Backing.Words.empty() ? Backing.Name : Backing.Words))
    return {};
  return " " + First->second.Readable;
}

void HighCWriter::writePointerBackings(const std::vector<va_t> &Deferred) {
  // Slots that hold addresses come last: they name the objects above, and
  // each other by the declarations that come first.
  const unsigned PointerBytes = pointerBytes(Opts.TheArch);
  auto WordCount = [&](const ImageBacking &Backing) {
    return (Backing.Pad + (Backing.End - Backing.Base) + PointerBytes - 1) /
           PointerBytes;
  };
  for (const ImageBacking &Backing : ImageBackings) {
    if (Backing.Words.empty())
      continue;
    OS << "union " << Backing.Words << "_words { ";
    if (Backing.Align > PointerBytes)
      OS << "_Alignas(" << Backing.Align << ") ";
    OS << "uintptr_t w[" << WordCount(Backing) << "]; unsigned char b["
       << WordCount(Backing) * PointerBytes << "]; };\n"
       << "extern union " << Backing.Words << "_words " << Backing.Words
       << ";\n";
  }
  for (const va_t Addr : Deferred) {
    const ImageObject &Obj = ImageObjects.at(Addr);
    OS << "extern " << declarationToC(Obj.Type, Obj.Name) << ";\n";
  }
  for (const va_t Addr : Deferred) {
    const ImageObject &Obj = ImageObjects.at(Addr);
    if (Opts.EmitComments)
      OS << "/* neverd.image: 0x" << llvm::utohexstr(Addr)
         << (Obj.Readable.empty() ? "" : " ") << Obj.Readable << " */\n";
    OS << declarationToC(Obj.Type, Obj.Name) << " = "
       << *relocatedSlotInitializer(Addr, Obj) << ";\n";
  }
  for (const ImageBacking &Backing : ImageBackings) {
    if (Backing.Words.empty())
      continue;
    if (Opts.Image->hasMachOAuthenticatedPointerIn(Backing.Base,
                                                   Backing.End - Backing.Base))
      continue;
    if (Opts.EmitComments)
      OS << "/* neverd.image: 0x" << llvm::utohexstr(Backing.Base) << " .. 0x"
         << llvm::utohexstr(Backing.End) << backingReadable(Backing) << " */\n";
    OS << "union " << Backing.Words << "_words " << Backing.Words
       << " = { .w = {";
    // A long initializer takes a few words a line, so no line runs to the
    // object's size.
    const uint64_t Words = WordCount(Backing);
    const bool Wrapped = Words > ImageWordsPerLine;
    for (uint64_t I = 0; I < Words; ++I) {
      // The words before the group pad it; they hold no byte of the image.
      const va_t Slot = Backing.Base - Backing.Pad + I * PointerBytes;
      if (Wrapped && I % ImageWordsPerLine == 0)
        OS << (I ? ",\n  " : "\n  ");
      else
        OS << (I ? ", " : " ");
      // The backing lists its slots in address order.
      if (std::binary_search(Backing.PointerSlots.begin(),
                             Backing.PointerSlots.end(), Slot))
        if (auto Target = relocatedSlotTarget(Slot)) {
          OS << *Target;
          continue;
        }
      // Bytes past the image's data, and outside the group, are zero.
      uint64_t Value = 0;
      for (unsigned Byte = 0; Byte < PointerBytes; ++Byte)
        if (Slot + Byte >= Backing.Base && Slot + Byte < Backing.End)
          if (const uint8_t *Data = Opts.Image->readVA(Slot + Byte, 1))
            Value |= uint64_t{*Data} << (8 * Byte);
      OS << (Value ? "0x" + llvm::utohexstr(Value) +
                         (PointerBytes == 8 ? "ull" : "u")
                   : std::string("0"));
    }
    OS << (Wrapped ? "\n} };\n" : " } };\n");
  }
}

std::optional<std::string> HighCWriter::stringLiteral(va_t Addr) const {
  auto [Known, Added] = StringLiterals.try_emplace(Addr);
  if (Added)
    Known->second = stringLiteralAt(Opts.Image, Dbg, Addr);
  return Known->second;
}

std::optional<std::string> HighCWriter::dataSymbolAddress(va_t Slot) const {
  const auto It = DataSymbolBindings.find(Slot);
  if (It == DataSymbolBindings.end())
    return std::nullopt;
  const auto &Binding = It->second;
  std::string Address;
  if (Binding.Definition) {
    if (auto Backing = imageBackingAddress(*Binding.Definition))
      Address = *Backing;
    else if (auto Name = imageObjectName(*Binding.Definition)) {
      const auto &Obj = ImageObjects.at(*Binding.Definition);
      Address = (Obj.String ? "" : "&") + *Name;
    } else
      return std::nullopt;
  } else {
    const auto Name = ExternalDataNames.find(Binding.Name);
    if (Name == ExternalDataNames.end())
      return std::nullopt;
    Address = Name->second;
  }
  std::string Value = "(uintptr_t)" + Address;
  if (Binding.Addend)
    Value =
        "(" + Value + " + (uintptr_t)(" + std::to_string(Binding.Addend) + "))";
  return Value;
}

std::optional<std::string> HighCWriter::relocatedSlotTarget(va_t Slot) const {
  if (auto Address = dataSymbolAddress(Slot))
    return Address;
  const unsigned PointerBytes = pointerBytes(Opts.TheArch);
  const uint8_t *Bytes = Opts.Image->readVA(Slot, PointerBytes);
  if (!Bytes)
    return std::nullopt;
  const va_t Target =
      PointerBytes == 8 ? readLE<uint64_t>(Bytes) : readLE<uint32_t>(Bytes);
  if (Opts.Image->CodePtrRelocSlots.count(Slot)) {
    // A Thumb function's address carries its mode in bit 0.
    for (const va_t Entry : {Target, Target & ~va_t{1}})
      if (auto It = FunctionAddressNames.find(Entry);
          It != FunctionAddressNames.end())
        return "(uintptr_t)&" + It->second + (Entry != Target ? " + 1" : "");
    return std::nullopt;
  }
  if (!Opts.Image->DataPtrRelocSlots.count(Slot))
    return std::nullopt;
  if (auto Backed = imageBackingAddress(Target))
    return "(uintptr_t)" + *Backed;
  if (auto Name = imageObjectName(Target)) {
    const auto Obj = ImageObjects.find(Target);
    const bool Array = Obj != ImageObjects.end() && Obj->second.String;
    return "(uintptr_t)" + std::string(Array ? "" : "&") + *Name;
  }
  if (auto Literal = stringLiteral(Target))
    return "(uintptr_t)" + *Literal;
  return std::nullopt;
}

std::string HighCWriter::relocatedTargetNote(va_t Slot) const {
  if (!Opts.Image->DataPtrRelocSlots.count(Slot))
    return {};
  const unsigned PointerBytes = pointerBytes(Opts.TheArch);
  const uint8_t *Bytes = Opts.Image->readVA(Slot, PointerBytes);
  if (!Bytes)
    return {};
  const va_t Target =
      PointerBytes == 8 ? readLE<uint64_t>(Bytes) : readLE<uint32_t>(Bytes);
  const ImageSymbolName Named = imageSymbolName(Target);
  // The listing names it by its symbol as the image spells it: the
  // underscore of `_IO_stdin_used` stays, though its C identifier drops it.
  if (!Named.Symbol.empty() && isPlainSymbolText(Named.Symbol) &&
      !llvm::StringRef(Named.Symbol).contains("*/"))
    return "&" + Named.Symbol;
  return Named.Raw.empty() ? std::string() : "&" + Named.Raw;
}

std::optional<std::string>
HighCWriter::relocatedSlotInitializer(va_t Addr, const ImageObject &Obj) const {
  const TypeRef &Type = Obj.Type;
  if (!Type || Type->Size != pointerBytes(Opts.TheArch) ||
      (Type->Kind != NdTypeKind::Int && Type->Kind != NdTypeKind::Ptr) ||
      (!Opts.Image->CodePtrRelocSlots.count(Addr) &&
       !Opts.Image->DataPtrRelocSlots.count(Addr) &&
       !DataSymbolBindings.count(Addr)))
    return std::nullopt;
  auto Target = relocatedSlotTarget(Addr);
  if (!Target || Type->Kind == NdTypeKind::Int)
    return Target;
  return "(" + typeToC(Type) + ")" + *Target;
}

std::optional<std::string>
HighCWriter::imageObjectInitializer(va_t Addr, const ImageObject &Obj) {
  const TypeRef &Type = Obj.Type;
  // C has no 128-bit literal; the two halves make a constant expression.
  if (Type && Type->Kind == NdTypeKind::Int && !Type->IsEnum &&
      Type->Size == 16) {
    const uint8_t *Bytes = Opts.Image->readVA(Addr, 16);
    if (!Bytes)
      return std::nullopt;
    uint64_t Low = 0, High = 0;
    for (unsigned I = 0; I < 8; ++I) {
      Low |= static_cast<uint64_t>(Bytes[I]) << (8 * I);
      High |= static_cast<uint64_t>(Bytes[8 + I]) << (8 * I);
    }
    if (!Low && !High)
      return std::nullopt;
    std::string Value = "0x" + llvm::utohexstr(Low) + "ull";
    if (High)
      Value = "(unsigned __int128)0x" + llvm::utohexstr(High) + "ull << 64 | " +
              Value;
    if (!Type->IsSigned)
      return Value;
    // A _BitInt(128) standing in for __int128 takes no __builtin_bit_cast in
    // a constant initializer; its conversion wraps the same bits.
    if (Int128AsBitInt)
      return "(" + typeToC(Type) + ")(" + Value + ")";
    return "__builtin_bit_cast(" + typeToC(Type) + ", (unsigned __int128)(" +
           Value + "))";
  }
  // A wider integer, a 256- or 512-bit constant vector, is the sum of its
  // 64-bit pieces shifted into place, a constant expression of the unsigned
  // type whose bits its conversion keeps.
  if (Type && Type->Kind == NdTypeKind::Int && !Type->IsEnum &&
      Type->Size > 16 && Type->Size % 8 == 0) {
    const uint8_t *Bytes = Opts.Image->readVA(Addr, Type->Size);
    if (!Bytes)
      return std::nullopt;
    const std::string Unsigned =
        typeToC(NdType::makeInt(Type->Size, /*Signed=*/false));
    std::string Value;
    for (unsigned Piece = Type->Size / 8; Piece-- > 0;) {
      uint64_t Bits = 0;
      for (unsigned I = 0; I < 8; ++I)
        Bits |= static_cast<uint64_t>(Bytes[Piece * 8 + I]) << (8 * I);
      if (!Bits)
        continue;
      std::string Term = "(" + Unsigned + ")0x" + llvm::utohexstr(Bits) + "ull";
      if (Piece)
        Term = "(" + Term + " << " + std::to_string(Piece * 64) + ")";
      Value += (Value.empty() ? "" : " | ") + Term;
    }
    if (Value.empty())
      return std::nullopt;
    return Type->IsSigned ? "(" + typeToC(Type) + ")(" + Value + ")" : Value;
  }
  if (!Type || (Type->Size != 1 && Type->Size != 2 && Type->Size != 4 &&
                Type->Size != 8))
    return std::nullopt;
  // Bytes the image does not hold are zero, as static storage starts.
  const uint8_t *Bytes = Opts.Image->readVA(Addr, Type->Size);
  if (!Bytes)
    return std::nullopt;
  uint64_t Value = 0;
  for (unsigned I = 0; I < Type->Size; ++I)
    Value |= static_cast<uint64_t>(Bytes[I]) << (8 * I);
  if (!Value)
    return std::nullopt;
  switch (Type->Kind) {
  case NdTypeKind::Int:
    return Type->IsEnum ? std::nullopt
                        : std::optional<std::string>(constStr(Value, Type));
  case NdTypeKind::Ptr:
    return "(" + typeToC(Type) + ")" + constStr(Value);
  case NdTypeKind::Float:
    return floatConstantText(Value, Type);
  default:
    return std::nullopt;
  }
}

std::optional<std::string>
HighCWriter::floatConstantText(uint64_t Value, const TypeRef &Type) const {
  if (!Type || Type->Kind != NdTypeKind::Float ||
      (Type->Size != 4 && Type->Size != 8))
    return std::nullopt;
  const llvm::APFloat Float(Type->Size == 4 ? llvm::APFloat::IEEEsingle()
                                            : llvm::APFloat::IEEEdouble(),
                            llvm::APInt(Type->Size * 8, Value));
  // C has no constant for infinities and NaNs, which take their bits.
  if (!Float.isFinite())
    return "__builtin_bit_cast(" + typeToC(Type) + ", 0x" +
           llvm::utohexstr(Value) + (Type->Size == 4 ? "u" : "ull") + ")";
  // The shortest decimal that reads back as the same bits: exact, the way
  // the source spelled it, and in no locale's notation.
  const bool Single = Type->Size == sizeof(float);
  char Buffer[64];
  const std::to_chars_result Written =
      Single ? std::to_chars(Buffer, Buffer + sizeof(Buffer),
                             Float.convertToFloat())
             : std::to_chars(Buffer, Buffer + sizeof(Buffer),
                             Float.convertToDouble());
  std::string Text(Buffer, Written.ptr);
  // `1` alone would be an integer constant.
  if (Text.find_first_of(".e") == std::string::npos)
    Text += ".0";
  return Text + (Single ? "f" : "");
}

void HighCWriter::writeX64SyscallHelper() {
  if (!NeedsX64SyscallHelper)
    return;
  // Linux x86-64 SYSCALL uses rax for the number, then rdi/rsi/rdx/r10/r8/r9
  // for arguments. It returns rax and writes the pre-entry flags to r11.
  OS << "#if !defined(__linux__) || !defined(__x86_64__)\n"
        "#error \"neverd_x64_syscall requires Linux x86-64\"\n"
        "#endif\n"
        "static inline unsigned __int128 neverd_x64_syscall(\n"
        "    uint64_t number, uint64_t arg1, unsigned __int128 arg2_3,\n"
        "    unsigned __int128 arg4_5, uint64_t arg6) {\n"
        "    uint64_t result = number;\n"
        "    register uint64_t in_r10 __asm__(\"r10\") = (uint64_t)arg4_5;\n"
        "    register uint64_t in_r8 __asm__(\"r8\") =\n"
        "        (uint64_t)(arg4_5 >> 64);\n"
        "    register uint64_t in_r9 __asm__(\"r9\") = arg6;\n"
        "    register uint64_t out_r11 __asm__(\"r11\");\n"
        "    __asm__ volatile(\"syscall\"\n"
        "        : \"+a\"(result), \"=r\"(out_r11)\n"
        "        : \"D\"(arg1), \"S\"((uint64_t)arg2_3),\n"
        "          \"d\"((uint64_t)(arg2_3 >> 64)), \"r\"(in_r10),\n"
        "          \"r\"(in_r8), \"r\"(in_r9)\n"
        "        : \"rcx\", \"memory\", \"cc\");\n"
        "    return ((unsigned __int128)out_r11 << 64) | result;\n"
        "}\n\n";
}

void HighCWriter::writeLinuxSyscallHelpers() {
  for (Intrinsic Id : LinuxSyscallHelpers)
    if (const SyscallConvention *Convention = modeledSyscallConvention(Id))
      OS << linuxSyscallHelperDefinition(*Convention, intrinsicCName(Id));
}

void HighCWriter::writeX64WindowsSyscallHelper() {
  if (!NeedsX64WindowsSyscallHelper)
    return;
  // The NT system service convention puts the service number in EAX and the
  // first four arguments in R10, RDX, R8 and R9; later arguments are read
  // from the stack, whose layout C does not reproduce.  The handler returns
  // RAX and may change every volatile register, so all of them come back.
  OS << "#if !defined(_WIN64) || !defined(__x86_64__)\n"
        "#error \"neverd_x64_windows_syscall requires Windows x86-64\"\n"
        "#endif\n"
        "static inline unsigned _BitInt(384) neverd_x64_windows_syscall(\n"
        "    uint64_t number, uint64_t arg1, uint64_t arg2, uint64_t arg3,\n"
        "    uint64_t arg4) {\n"
        "    uint64_t rax = number, rdx = arg2;\n"
        "    register uint64_t r10 __asm__(\"r10\") = arg1;\n"
        "    register uint64_t r8 __asm__(\"r8\") = arg3;\n"
        "    register uint64_t r9 __asm__(\"r9\") = arg4;\n"
        "    register uint64_t r11 __asm__(\"r11\");\n"
        "    __asm__ volatile(\"syscall\"\n"
        "        : \"+a\"(rax), \"+d\"(rdx), \"+r\"(r10), \"+r\"(r8), "
        "\"+r\"(r9), \"=r\"(r11)\n"
        "        :\n"
        "        : \"rcx\", \"memory\", \"cc\");\n"
        "    unsigned _BitInt(384) result = r10;\n"
        "    result = (result << 64) | r9;\n"
        "    result = (result << 64) | r8;\n"
        "    result = (result << 64) | rdx;\n"
        "    result = (result << 64) | r11;\n"
        "    return (result << 64) | rax;\n"
        "}\n\n";
}

void HighCWriter::writeAll(const std::vector<HighFunc> &Funcs) {
  // The unit spells the records it declares no typedef of, as a function or
  // an object has the name, by their tags.
  const auto TagKeyword = [this](llvm::StringRef Name) {
    return recordTagKeyword(Name);
  };
  CRecordTagSpelling Tags(TagKeyword);
  prepareRecordDefinitions(Funcs);
  prepareFunctionIdentifiers(Funcs);
  collectImageObjects(Funcs);
  collectMemoryTypes(Funcs);
  discoverHiddenCxxThrowCtors(Funcs);
  writeIncludes(Funcs);
  std::set<std::string> Records;
  std::function<void(const TypeRef &)> RecordType = [&](const TypeRef &Type) {
    if (!Type || Type->Kind != NdTypeKind::Struct)
      return;
    const auto Name = typeToC(Type);
    if (!Records.insert(Name).second)
      return;
    for (const auto &Field : Type->Fields)
      RecordType(Field);
    std::string Guard;
    if (Opts.EmitRecordGuards) {
      Guard = "NEVERD_SOURCE_" + llvm::StringRef(Name).drop_front(7).upper();
      OS << "#ifndef " << Guard << "\n#define " << Guard << "\n";
    }
    OS << Name << " {\n";
    for (size_t I = 0; I < Type->Fields.size(); ++I)
      OS << "    "
         << declarationToC(Type->Fields[I], "field_" + std::to_string(I))
         << ";\n";
    OS << "};\n_Static_assert(sizeof(" << Name << ") == " << Type->Size
       << ", \"source record size\");\n_Static_assert(_Alignof(" << Name
       << ") == " << Type->Alignment << ", \"source record alignment\");\n";
    for (size_t I = 0; I < Type->Fields.size(); ++I)
      OS << "_Static_assert(__builtin_offsetof(" << Name << ", field_" << I
         << ") == " << Type->FieldOffsets[I]
         << ", \"source record offset\");\n";
    if (Opts.EmitRecordGuards)
      OS << "#endif\n";
  };
  std::set<const HighExpr *> Seen;
  std::function<void(const ExprPtr &)> Visit = [&](const ExprPtr &E) {
    if (!E || !Seen.insert(E.get()).second)
      return;
    RecordType(E->Type);
    if (E->SourceCallHint) {
      RecordType(E->SourceCallHint->Signature.ReturnType);
      for (const auto &P : E->SourceCallHint->Signature.Parameters)
        RecordType(P.Type);
    }
    for (const auto &Child : E->Operands)
      Visit(Child);
  };
  for (const auto &Func : Funcs) {
    RecordType(Func.ReturnType);
    for (const auto &P : Func.Params)
      RecordType(P.Type);
    walkStmts(Func.Body,
              [&](const HighStmt &Stmt) { forEachExpr(Stmt, Visit); });
  }
  writeX87CHelpers(OS, UsesX87Extended, X87Helpers);
  writeX87ContextHelpers(Funcs);
  writeX86FPStateCHelpers(OS, X86FPStateHelpers);
  writeMemoryHelpers();
  writeX64SyscallHelper();
  writeX64WindowsSyscallHelper();
  writeLinuxSyscallHelpers();
  writeRegistrationEntryDeclarations(Funcs);
  writeForwardDecls(Funcs);
  writeImageObjects();

  for (size_t I = 0; I < Funcs.size(); ++I) {
    if (Funcs[I].Name.empty())
      continue;
    auto Event = SourceRecorder ? SourceRecorder->function(Funcs[I].Entry)
                                : std::nullopt;
    if (SourceRecorder && SourceRecorder->recordsPositions())
      SourceRecorder->positionDefinition(Funcs[I].Entry, OS.tell());
    else if (SourceRecorder)
      OS << SourceRecorder->definition(Funcs[I].Entry);
    if (Event)
      OS << SourceRecorder->begin(*Event);
    writeFunction(Funcs[I]);
    if (Event)
      OS << SourceRecorder->end(*Event);
    if (I + 1 < Funcs.size())
      OS << "\n";
  }
  recordSourceNames(Funcs);
}

//===----------------------------------------------------------------------===//
// Public API
//===----------------------------------------------------------------------===//

HighCImageDataObjects collectImageDataObjects(const BinaryImage &Image) {
  HighCImageDataObjects Objects;
  auto &SizedObjects = Objects.SizedObjects;
  for (const Symbol &Sym : Image.Symbols)
    if (!Sym.IsFunc && Sym.Size && isImageDataAt(Image, Sym.Addr) &&
        Sym.Size <= std::numeric_limits<va_t>::max() - Sym.Addr)
      SizedObjects.emplace_back(Sym.Addr, Sym.Size);
  llvm::sort(SizedObjects, [](const auto &A, const auto &B) {
    return A.first != B.first ? A.first < B.first : A.second > B.second;
  });
  SizedObjects.erase(std::unique(SizedObjects.begin(), SizedObjects.end(),
                                 [](const auto &A, const auto &B) {
                                   return A.first == B.first;
                                 }),
                     SizedObjects.end());
  auto &SizedObjectReach = Objects.SizedObjectReach;
  for (const auto &[Addr, Size] : SizedObjects)
    SizedObjectReach.push_back(
        std::max(SizedObjectReach.empty() ? va_t{0} : SizedObjectReach.back(),
                 Addr + Size));
  auto &DataSymbolStarts = Objects.DataSymbolStarts;
  for (const Symbol &Sym : Image.Symbols)
    if (!Sym.IsFunc && isImageDataAt(Image, Sym.Addr))
      DataSymbolStarts.push_back(Sym.Addr);
  llvm::sort(DataSymbolStarts);
  DataSymbolStarts.erase(
      std::unique(DataSymbolStarts.begin(), DataSymbolStarts.end()),
      DataSymbolStarts.end());
  return Objects;
}

HighCImageFunctionSymbols
collectImageFunctionSymbols(const BinaryImage &Image) {
  HighCImageFunctionSymbols Symbols;
  for (const Symbol &Sym : Image.Symbols)
    if (Sym.IsFunc && !Sym.Name.empty())
      Symbols.try_emplace(Sym.Addr, &Sym);
  return Symbols;
}

void HighCEmitter::prepareImageFunctionNames(const BinaryImage &Image) {
  PreparedImage = nullptr;
  PreparedImageFunctionNames = collectImageFunctionNameViews(Image);
  PreparedDataObjects = collectImageDataObjects(Image);
  PreparedFunctionSymbols = collectImageFunctionSymbols(Image);
  PreparedImage = &Image;
}

bool HighCEmitter::emit(const std::vector<HighFunc> &Funcs,
                        llvm::raw_ostream &Out, const CEmitterOptions &Opts,
                        DebugContext *Dbg) {
  if (Opts.SourceMap)
    Opts.SourceMap->Variables.clear();
  // Check the complete supplied IR before source-only hiding or preparation
  // can discard an unreachable or malformed environment requirement.
  std::set<const HighExpr *> Seen;
  std::function<void(const ExprPtr &)> CheckRequirement =
      [&](const ExprPtr &Expr) {
        if (!Expr || !Seen.insert(Expr.get()).second)
          return;
        if (Expr->IntrinsicId == Intrinsic::RawEnvironmentRequirementV1)
          throw std::invalid_argument(
              "raw environment requirement has no C runtime contract");
        Expr->forEachChildExpr(CheckRequirement);
      };
  for (const auto &Func : Funcs)
    walkStmts(Func.Body, [&](const HighStmt &Stmt) {
      forEachExpr(Stmt, CheckRequirement);
      for (const auto &Clause : Stmt.EHClauses)
        CheckRequirement(Clause.FilterValue);
    });
  for (const auto &Func : Funcs)
    if (Func.SourceTypeHint &&
        (hasIndirectSourceParameters(*Func.SourceTypeHint) ||
         (hasSwiftErrorResult(*Func.SourceTypeHint) &&
          !isSwiftErrorEntryProjected(Func, Opts.TheArch))))
      throw std::invalid_argument("Indirect record or error-register source "
                                  "entries need a projection proof");
  // The records the code reads members of through others, as
  // `l->head->child` reads a node, which only a rendering finds: each
  // rendering asks the local types for those an earlier one found.
  std::vector<TypeRef> Extra;
  std::set<std::string> Asked;
  auto Render = [&](llvm::raw_ostream &OS, CSourceRecorder *Recorder,
                    std::vector<TypeRef> *Wanted = nullptr) {
    std::vector<HighFunc> Working = Funcs;
    if (Opts.StructuredExceptionSyntax) {
      attachCxxFuncletBodies(Working);
      for (HighFunc &Func : Working) {
        attachCxxObjectActionCalls(Func, Opts.TheArch);
        attachItaniumHandlerBodies(Func);
      }
    }
    const bool Prepared = Opts.Image && Opts.Image == PreparedImage;
    HighCWriter W(OS, Opts, Dbg, true,
                  Prepared ? &PreparedImageFunctionNames : nullptr, Recorder,
                  Prepared ? &PreparedDataObjects : nullptr,
                  Prepared ? &PreparedFunctionSymbols : nullptr);
    W.ExtraRecords = Extra;
    W.WantedRecords = Wanted;
    W.prepareFunctionReturns(Working);
    W.writeAll(Working);
  };
  // Whether rendering number \p Pass notes records for the next; and whether
  // \p Wanted, what one noted, asks for a record none asked for before.
  const auto Collects = [&](unsigned Pass) {
    return Opts.RecordDefinitions && Opts.EmitIncludes &&
           Pass + 1 < limits::kMaxHighCRecordDefinitionRenders;
  };
  const auto Grew = [&](std::vector<TypeRef> &Wanted) {
    bool Any = false;
    for (TypeRef &Record : Wanted)
      if (Asked.insert(Record->SourceName).second) {
        Extra.push_back(std::move(Record));
        Any = true;
      }
    return Any;
  };
  if (!Opts.SourceMap) {
    std::string Text;
    for (unsigned Pass = 0;; ++Pass) {
      Text.clear();
      llvm::raw_string_ostream TextOS(Text);
      std::vector<TypeRef> Wanted;
      Render(TextOS, nullptr, Collects(Pass) ? &Wanted : nullptr);
      if (!Grew(Wanted))
        break;
    }
    Out << Text;
    return true;
  }
  std::string Ordinary;
  std::optional<CSourceRecorder> Positions;
  for (unsigned Pass = 0;; ++Pass) {
    Ordinary.clear();
    llvm::raw_string_ostream OrdinaryOS(Ordinary);
    Positions.reset();
    if (!Opts.SourceMap->Recognitions ||
        Opts.SourceMap->Recognitions->empty()) {
      try {
        Positions.emplace(*Opts.SourceMap, llvm::StringRef(),
                          CSourceRecorder::Recording::Positions);
        Positions->prepareHighSources();
      } catch (const std::exception &) {
        Positions.reset();
      }
    }
    // Position recording adds no bytes and cannot influence expression
    // spelling.  Render failures are ordinary source failures, not optional
    // mapping failures.
    std::vector<TypeRef> Wanted;
    Render(OrdinaryOS, Positions ? &*Positions : nullptr,
           Collects(Pass) ? &Wanted : nullptr);
    if (!Grew(Wanted))
      break;
  }
  if (Positions && Positions->finishPositions(Ordinary)) {
    Out << Ordinary;
    return true;
  }
  if (!Opts.SourceMap->Recognitions || Opts.SourceMap->Recognitions->empty()) {
    // Without library evidence there is no independent coverage proof to
    // protect. Verify definition and instruction locations together instead
    // of repeating every source analysis for a library render with no events.
    CSourceRecorder Combined(
        *Opts.SourceMap, Ordinary,
        CSourceRecorder::Recording::DefinitionsAndInstructions);
    bool Mapped = false;
    try {
      Combined.prepareHighSources();
      std::string Annotated;
      llvm::raw_string_ostream AnnotatedOS(Annotated);
      Render(AnnotatedOS, &Combined);
      Mapped = Combined.finish(Annotated, Ordinary);
    } catch (const std::exception &) {
      // Independent recordings below can still recover either mapping.
    }
    if (Mapped) {
      Out << Ordinary;
      return true;
    }
    // A failed combined proof must not lose a definition or instruction map
    // that the original, independent recordings could have established.
  }
  CSourceRecorder Recorder(*Opts.SourceMap, Ordinary);
  try {
    Recorder.prepareHighSources();
    std::string Annotated;
    llvm::raw_string_ostream AnnotatedOS(Annotated);
    Render(AnnotatedOS, &Recorder);
    Recorder.finish(Annotated, Ordinary);
  } catch (const std::exception &) {
    // Mapping is optional. A failed marked rendering cannot invalidate the
    // ordinary source that was already emitted successfully above.
  }
  // Statement markers must not affect expression spelling or the independent
  // library-region coverage proof. Each private render must reproduce Ordinary.
  CSourceRecorder Instructions(*Opts.SourceMap, Ordinary,
                               CSourceRecorder::Recording::Instructions);
  try {
    Instructions.prepareHighSources();
    std::string Annotated;
    llvm::raw_string_ostream OS(Annotated);
    Render(OS, &Instructions);
    Instructions.finish(Annotated, Ordinary);
  } catch (const std::exception &) {
    // Missing navigation evidence leaves the successful source and library map.
  }
  Out << Ordinary;
  return true;
}

bool HighCEmitter::emitToFile(const std::vector<HighFunc> &Funcs,
                              const std::string &Path,
                              const CEmitterOptions &Opts, DebugContext *Dbg) {
  std::error_code EC;
  llvm::raw_fd_ostream OS(Path, EC);
  if (EC) {
    llvm::WithColor::error() << "high_c_emitter: cannot open " << Path << ": "
                             << EC.message() << "\n";
    return false;
  }
  bool Ok = emit(Funcs, OS, Opts, Dbg);
  LLVM_DEBUG(llvm::dbgs() << "high_c_emitter: written to " << Path << "\n");
  return Ok;
}

} // namespace neverd
