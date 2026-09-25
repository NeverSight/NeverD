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

#include "HighCWriter.h"

#define DEBUG_TYPE "neverd-highc-emitter"
#include "neverd/backend/llvm/LLVMX86AddressSpaces.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <functional>
#include <map>
#include <set>

namespace neverd {

namespace {

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

std::string memoryHelperName(llvm::StringRef Operation, unsigned TypeIndex,
                             NdMemoryOrdering Ordering,
                             NdMemoryAddressSpace AddressSpace) {
  std::string Name = "neverd_mem_" + Operation.str() + "_";
  if (AddressSpace != NdMemoryAddressSpace::Default)
    Name += std::string(memoryAddressSpaceName(AddressSpace)) + "_";
  if (Ordering != NdMemoryOrdering::None)
    Name += std::string(memoryOrderingName(Ordering)) + "_";
  return Name + std::to_string(TypeIndex);
}

std::string memoryPointerCast(llvm::StringRef Type, llvm::StringRef Address,
                              NdMemoryAddressSpace AddressSpace, bool IsConst) {
  std::string Qualified = IsConst ? "const " : "";
  Qualified += Type.str();
  if (AddressSpace != NdMemoryAddressSpace::Default)
    Qualified += " __attribute__((address_space(" +
                 std::to_string(cMemoryAddressSpace(AddressSpace)) + ")))";
  return "(" + Qualified + " *)(uintptr_t)(" + Address.str() + ")";
}

} // anonymous namespace

void HighCWriter::prepareFunctionIdentifiers(
    const std::vector<HighFunc> &Funcs) {
  GlobalIdentifierAllocator = CProjectionIdentifierAllocator{};
  FunctionIdentifiers.clear();
  FunctionIdentifiersBySourceName.clear();
  ExternalFunctionIdentifiers.clear();
  DefinedFuncs.clear();
  DefinedFunctionsByIdentifier.clear();
  DefinedFunctionsByAddress.clear();

  // Imported runtime veneers are ordinary discovered functions, and can
  // therefore carry the same source spelling as the external API they jump
  // to.  Give the linked API first choice of that spelling so the veneer gets
  // a distinct C identifier instead of recursively calling itself.
  std::set<std::string> LinkedRuntimeNames;
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
            SourceFunctionTypeHint::OriginKind::DarwinSDK)
          Name = "neverd_darwin_" + Hint.TargetName;
        else
          Name = Hint.TargetName;
      }
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

  for (const HighFunc &Func : Funcs) {
    if (Func.Name.empty())
      continue;
    DefinedFuncs[Func.Name] = &Func;
    if (Func.Name.front() == '_')
      DefinedFuncs[Func.Name.substr(1)] = &Func;
    if (Func.Entry) {
      auto [It, Added] = DefinedFunctionsByAddress.emplace(Func.Entry, &Func);
      if (!Added)
        It->second = nullptr;
    }
    llvm::StringRef SourceName(Func.Name);
    llvm::StringRef RenderedName = SourceName;
    RenderedName.consume_front("_");
    std::string Identifier =
        GlobalIdentifierAllocator.allocate(RenderedName, "nd_function");
    FunctionIdentifiers.emplace(&Func, Identifier);
    DefinedFunctionsByIdentifier.emplace(Identifier, &Func);
    FunctionIdentifiersBySourceName.try_emplace(SourceName.str(), Identifier);
    FunctionIdentifiersBySourceName.try_emplace(RenderedName.str(), Identifier);
  }
}

std::string HighCWriter::functionIdentifier(const HighFunc &Func) const {
  if (auto It = FunctionIdentifiers.find(&Func);
      It != FunctionIdentifiers.end())
    return It->second;
  llvm::StringRef Name(Func.Name);
  Name.consume_front("_");
  return canonicalizeCProjectionIdentifier(Name, "nd_function");
}

std::string HighCWriter::functionIdentifier(llvm::StringRef SourceName) const {
  if (auto It = FunctionIdentifiersBySourceName.find(SourceName.str());
      It != FunctionIdentifiersBySourceName.end())
    return It->second;
  if (auto It = ExternalFunctionIdentifiers.find(SourceName.str());
      It != ExternalFunctionIdentifiers.end())
    return It->second;
  SourceName.consume_front("_");
  return canonicalizeCProjectionIdentifier(SourceName, "nd_function");
}

std::string HighCWriter::memoryTypeName(const TypeRef &Ty) const {
  std::string Name = typeToC(Ty);
  return Name == "void" ? "uint32_t" : Name;
}

void HighCWriter::collectMemoryTypes(const std::vector<HighFunc> &Funcs) {
  std::set<std::string> Names;
  PartialIntegerBytes.clear();
  SegmentedMemoryTypes.clear();
  AtomicLoadTypes.clear();
  AtomicStoreTypes.clear();
  HasSegmentedMemory = false;
  Has256BitInteger = false;
  Has512BitInteger = false;
  // Native AArch64 vector carriers are projected to SVE ACLE types.  x86
  // vector intrinsics use scalar integer carriers at the HighIR boundary.
  const bool ProjectsScalarWideIntegers =
      Opts.TheArch == Arch::X86 || Opts.TheArch == Arch::X64;
  auto CollectWideType = [&](const TypeRef &Type) {
    if (const unsigned Bytes = partialIntegerBytes(Type))
      PartialIntegerBytes.emplace(typeToC(Type), Bytes);
    Has256BitInteger |= ProjectsScalarWideIntegers && Type &&
                        Type->Kind == NdTypeKind::Int && Type->Size == 32;
    Has512BitInteger |= ProjectsScalarWideIntegers && Type &&
                        Type->Kind == NdTypeKind::Int && Type->Size == 64;
  };
  std::set<const HighExpr *> Seen;
  bool HideEHRuntimeMemory = false;
  std::function<void(const HighExpr &)> Visit = [&](const HighExpr &E) {
    if (!Seen.insert(&E).second)
      return;
    if (HideEHRuntimeMemory &&
        E.MemoryAddressSpace == NdMemoryAddressSpace::X86FS)
      return;
    CollectWideType(E.Type);
    CollectWideType(E.CastTo);
    const bool MsvcSegmentedScalar =
        E.MemoryOrdering == NdMemoryOrdering::None &&
        E.MemoryAddressSpace != NdMemoryAddressSpace::Default &&
        (Opts.TheArch == Arch::X86 || Opts.TheArch == Arch::X64) &&
        (E.Kind == ExprKind::Load || E.Kind == ExprKind::Store);
    if (E.MemoryAddressSpace != NdMemoryAddressSpace::Default) {
      validateMemoryAddressSpaceForC(E.MemoryAddressSpace, Opts.TheArch);
      if (!MsvcSegmentedScalar)
        HasSegmentedMemory = true;
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
      const TypeRef AddressType =
          !E.Operands.empty() && E.Operands[0] ? E.Operands[0]->Type : nullptr;
      const bool DirectTypedLoad =
          OrdinaryMemory && partialIntegerBytes(E.Type) == 0 && AddressType &&
          AddressType->Kind == NdTypeKind::Ptr && AddressType->Pointee &&
          equalSourceTypes(AddressType->Pointee, E.Type);
      // Ordinary default-address-space loads print as `*(T *)addr` or a
      // named frame slot.  Helpers are only required for atomics, segmented
      // memory, or partial integer widths (including synthetic-frame fallback).
      if (!MsvcSegmentedScalar && !DirectTypedLoad) {
        const bool NeedsHelper =
            !OrdinaryMemory || partialIntegerBytes(E.Type) != 0;
        if (NeedsHelper)
          Names.insert(Type);
      }
      if (E.MemoryAddressSpace != NdMemoryAddressSpace::Default &&
          !MsvcSegmentedScalar)
        SegmentedMemoryTypes.insert({Type, E.MemoryAddressSpace});
      if (E.MemoryOrdering != NdMemoryOrdering::None)
        AtomicLoadTypes.insert({Type, E.MemoryOrdering, E.MemoryAddressSpace});
    }
    if (E.Kind == ExprKind::Store && E.Operands.size() >= 2) {
      std::string Type = memoryTypeName(E.Operands[1]->Type);
      validateMemoryAddressSpaceForC(E.MemoryAddressSpace, Opts.TheArch);
      const bool NeedsHelper =
          partialIntegerBytes(E.Operands[1]->Type) != 0 ||
          E.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
          E.MemoryOrdering != NdMemoryOrdering::None;
      if (NeedsHelper)
        Names.insert(Type);
      if (E.MemoryAddressSpace != NdMemoryAddressSpace::Default)
        SegmentedMemoryTypes.insert({Type, E.MemoryAddressSpace});
      if (E.MemoryOrdering != NdMemoryOrdering::None)
        AtomicStoreTypes.insert({Type, E.MemoryOrdering, E.MemoryAddressSpace});
    }
    for (const ExprPtr &Operand : E.Operands)
      if (Operand)
        Visit(*Operand);
  };

  for (const HighFunc &Func : Funcs) {
    CurrentFunc = &Func;
    collectNamedFrameSlots(Func);
    CollectWideType(Func.ReturnType);
    for (const HighParam &Param : Func.Params)
      CollectWideType(Param.Type);
    for (const HighLocal &Local : Func.Locals)
      CollectWideType(Local.Type);
    HideEHRuntimeMemory = Func.ExceptionMetadata.has_value();
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
        const bool NeedsHelper =
            partialIntegerBytes(Stmt.StoreVal->Type) != 0 ||
            Stmt.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
            Stmt.MemoryOrdering != NdMemoryOrdering::None;
        if (NeedsHelper)
          Names.insert(Type);
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
        const bool NeedsHelper =
            partialIntegerBytes(Stmt.Dst->Type) != 0 ||
            Stmt.Dst->MemoryAddressSpace != NdMemoryAddressSpace::Default ||
            Stmt.Dst->MemoryOrdering != NdMemoryOrdering::None;
        if (NeedsHelper)
          Names.insert(Type);
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

  MemoryTypes.clear();
  unsigned Index = 0;
  for (const std::string &Name : Names)
    MemoryTypes.emplace(Name, Index++);
}

void HighCWriter::writeMemoryHelpers() {
  if (HasSegmentedMemory)
    OS << "#if !defined(__clang__)\n"
          "#error \"segmented-memory output requires Clang target address "
          "spaces\"\n"
          "#endif\n\n";

  auto WriteHelpers = [&](const std::string &Type, unsigned Index,
                          NdMemoryAddressSpace AddressSpace) {
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
        memoryHelperName("load", Index, NdMemoryOrdering::None, AddressSpace);
    const auto StoreName =
        memoryHelperName("store", Index, NdMemoryOrdering::None, AddressSpace);
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
  for (const auto &[Type, Index] : MemoryTypes)
    WriteHelpers(Type, Index, NdMemoryAddressSpace::Default);
  for (const auto &[Type, AddressSpace] : SegmentedMemoryTypes) {
    validateMemoryAddressSpaceForC(AddressSpace, Opts.TheArch);
    WriteHelpers(Type, MemoryTypes.at(Type), AddressSpace);
  }

  for (const auto &[Type, Ordering, AddressSpace] : AtomicLoadTypes) {
    if (PartialIntegerBytes.count(Type))
      llvm::report_fatal_error(
          "HighC atomic access requires a supported machine width");
    validateAtomicLoadOrdering(Ordering);
    validateMemoryAddressSpaceForC(AddressSpace, Opts.TheArch);
    unsigned Index = MemoryTypes.at(Type);
    OS << "static inline " << Type << " "
       << memoryHelperName("load", Index, Ordering, AddressSpace)
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
    unsigned Index = MemoryTypes.at(Type);
    OS << "static inline " << Type << " "
       << memoryHelperName("store", Index, Ordering, AddressSpace)
       << "(uintptr_t address, " << Type << " value) {\n"
       << "    __atomic_store_n("
       << memoryPointerCast(Type, "address", AddressSpace, false) << ", value, "
       << atomicOrderingToken(Ordering) << ");\n"
       << "    return value;\n"
       << "}\n\n";
  }
}

std::string
HighCWriter::memoryLoadExpr(const TypeRef &Ty, llvm::StringRef Addr,
                            NdMemoryOrdering Ordering,
                            NdMemoryAddressSpace AddressSpace) const {
  validateMemoryAddressSpaceForC(AddressSpace, Opts.TheArch);
  std::string Type = memoryTypeName(Ty);
  if (Ordering == NdMemoryOrdering::None &&
      AddressSpace == NdMemoryAddressSpace::Default &&
      partialIntegerBytes(Ty) == 0)
    return "(*(" + Type + " *)(" + Addr.str() + "))";
  if (std::string Seg = renderX86MsvcSegmentedLoad(
          Opts.TheArch, Ty ? Ty->Size : 0, Addr, Ordering, AddressSpace);
      !Seg.empty())
    return Seg;
  auto It = MemoryTypes.find(Type);
  if (It == MemoryTypes.end())
    llvm::report_fatal_error("HighC memory load type was not collected");
  unsigned Index = It->second;
  if (Ordering != NdMemoryOrdering::None)
    validateAtomicLoadOrdering(Ordering);
  return memoryHelperName("load", Index, Ordering, AddressSpace) +
         "((uintptr_t)(" + Addr.str() + "))";
}

std::string
HighCWriter::memoryStoreExpr(const TypeRef &Ty, llvm::StringRef Addr,
                             llvm::StringRef Val, NdMemoryOrdering Ordering,
                             NdMemoryAddressSpace AddressSpace) const {
  validateMemoryAddressSpaceForC(AddressSpace, Opts.TheArch);
  std::string Type = memoryTypeName(Ty);
  const std::string Value = Ty && Ty->Kind == NdTypeKind::Ptr
                                ? "(" + Type + ")(uintptr_t)(" + Val.str() + ")"
                                : Val.str();
  if (Ordering == NdMemoryOrdering::None &&
      AddressSpace == NdMemoryAddressSpace::Default &&
      partialIntegerBytes(Ty) == 0)
    return "(*(" + Type + " *)(" + Addr.str() + ") = " + Value + ")";
  auto It = MemoryTypes.find(Type);
  if (It == MemoryTypes.end())
    llvm::report_fatal_error("HighC memory store type was not collected");
  unsigned Index = It->second;
  if (Ordering != NdMemoryOrdering::None)
    validateAtomicStoreOrdering(Ordering);
  return memoryHelperName("store", Index, Ordering, AddressSpace) +
         "((uintptr_t)(" + Addr.str() + "), " + Value + ")";
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
         memoryPointerCast(Type, Addr, AddressSpace, false) + ", (" + Type +
         ")(" + Val.str() + "), " + atomicOrderingToken(Ordering) + ")";
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
         memoryPointerCast(Type, Addr, AddressSpace, false) + ", (" + Type +
         ")(" + Val.str() + "), " + atomicOrderingToken(Ordering) + ")";
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
  return "({ " + Type + " neverd_expected = (" + Type + ")(" + Expected.str() +
         "); (void)__atomic_compare_exchange_n(" +
         memoryPointerCast(Type, Addr, AddressSpace, false) +
         ", &neverd_expected, (" + Type + ")(" + Desired.str() + "), 0, " +
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
            Hint.CallKind == SourceCallTypeHint::Kind::DarwinRuntimeCall &&
            Hint.Signature.Origin ==
                SourceFunctionTypeHint::OriginKind::DarwinSDK;
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
          } else
            NeedsDarwinStackGuard |= Hint.TargetName == "__stack_chk_guard";
        } else if (Hint.CallKind ==
                   SourceCallTypeHint::Kind::SwiftStringBridge) {
          NeedsSwiftStringBridge = true;
        } else if (Hint.CallKind ==
                   SourceCallTypeHint::Kind::SwiftStringFromNSString) {
          NeedsSwiftStringFromNSString = true;
        } else if (Hint.CallKind ==
                   SourceCallTypeHint::Kind::SwiftValueWitness) {
          // The expression reloads the required witness from its runtime
          // metadata argument and therefore needs no linked declaration.
        } else if (Hint.CallKind == SourceCallTypeHint::Kind::Native ||
                   Hint.CallKind == SourceCallTypeHint::Kind::ObjCRuntimeCall ||
                   Hint.CallKind ==
                       SourceCallTypeHint::Kind::SwiftRuntimeCall ||
                   DeclaredC) {
          const bool Runtime =
              Hint.CallKind == SourceCallTypeHint::Kind::ObjCRuntimeCall ||
              Hint.CallKind == SourceCallTypeHint::Kind::SwiftRuntimeCall ||
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
          if (!Runtime)
            if (const auto *Definition =
                    sourceCallDefinition(Hint, ResolvedName))
              ResolvedName = functionIdentifier(*Definition);
          llvm::StringRef Name(ResolvedName);
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
                &Hint.Signature,
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
                       SourceCallTypeHint::Kind::RuntimeReadOnlyBytes) {
          if (Hint.TargetAddress)
            SourceObjectAddressHelpers.insert(
                "neverd_borrowed_bytes_" +
                llvm::utohexstr(Hint.TargetAddress, true) + "_" +
                std::to_string(Hint.ByteCount) + "_address");
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
                   SourceCallTypeHint::Kind::RuntimeStaticIdentity) {
          if (Hint.TargetAddress)
            SourceObjectAddressHelpers.insert(
                "neverd_static_identity_" +
                llvm::utohexstr(Hint.TargetAddress, true) + "_address");
        } else if (Hint.CallKind ==
                   SourceCallTypeHint::Kind::RuntimeLocalStorageAddress) {
          if (Hint.TargetAddress)
            SourceObjectAddressHelpers.insert(
                "neverd_local_storage_" +
                llvm::utohexstr(Hint.TargetAddress, true) + "_address");
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
        } else if (Hint.CallKind != SourceCallTypeHint::Kind::BlockInvoke) {
          NeedsObjCRuntime = true;
          NeedsObjCSuper2 |=
              Hint.CallKind == SourceCallTypeHint::Kind::ObjCSuper2;
        }
        for (const auto &Operand : Ex.Operands)
          if (Operand)
            Visit(*Operand);
        return;
      }
      if (Ex.IntrinsicId == Intrinsic::A64_Frinti)
        NeedsFEnvAccess = true;
      if (Ex.IntrinsicId != Intrinsic::None && intrinsicCName(Ex.IntrinsicId))
        HasCIntrinsics = true;
      std::string Name = Ex.CallTarget;
      if (!Name.empty()) {
        if (Ex.IntrinsicId != Intrinsic::None) {
          CIntrinsicNames.insert(Name);
        }
        if (Ex.IntrinsicId == Intrinsic::None && Name[0] == '_')
          Name = Name.substr(1);
        if (!isMsvcCxxThrowCallName(Name) &&
            !isMsvcCxxThrowCallName(Ex.CallTarget))
          Targets.insert(Name);
      }
    }
    for (auto &Op : Ex.Operands)
      if (Op)
        Visit(*Op);
  };
  Visit(Expr);
}

void HighCWriter::collectCallTargets(const std::vector<HighStmt> &Stmts,
                                     std::set<std::string> &Targets) {
  for (auto &S : Stmts) {
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
  if (!ImageFunctionNames) {
    ImageFunctionNames.emplace();
    for (const Symbol &Sym : Opts.Image->Symbols)
      if (Sym.IsFunc && !Sym.Name.empty() &&
          !Opts.Image->findImportAt(Sym.Addr))
        ImageFunctionNames->insert(Sym.Name);
  }
  return ImageFunctionNames->count(Name.str()) ||
         ImageFunctionNames->count(Clean.str()) ||
         ImageFunctionNames->count(("_" + Clean).str());
}

void HighCWriter::writeIncludes(const std::vector<HighFunc> &Funcs) {
  if (!Opts.EmitIncludes) {
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
  if (!MemoryTypes.empty())
    Headers.insert("string.h");

  std::set<std::string> CallTargets;
  // An analysis-only function prints as ordinary C too, so its callees need
  // the same headers and prototypes.
  for (auto &F : Funcs)
    collectCallTargets(F.Body, CallTargets);

  for (auto &Name : CallTargets) {
    if (isOwnFunctionName(Name, Funcs))
      continue;
    if (const char *Hdr = libc::headerFor(Name))
      Headers.insert(Hdr);
  }
  if (NeedsObjCRuntime) {
    Headers.insert("objc/message.h");
    Headers.insert("objc/runtime.h");
  }
  if (NeedsDarwinLocks)
    Headers.insert("os/lock.h");
  if (NeedsDarwinBlocks)
    Headers.insert("Block.h");

  if (HasCIntrinsics)
    for (const char *Hdr : getArchIntrinsicHeaders(Opts.TheArch))
      Headers.insert(Hdr);

  for (auto &H : Headers)
    OS << "#include <" << H << ">\n";
  if (NeedsFEnvAccess)
    OS << "#pragma STDC FENV_ACCESS ON\n";
  OS << "\n";
  if (Has256BitInteger)
    OS << "typedef unsigned _BitInt(256) uint256_t;\n"
          "typedef _BitInt(256) int256_t;\n\n";
  if (Has512BitInteger)
    OS << "typedef unsigned _BitInt(512) uint512_t;\n"
          "typedef _BitInt(512) int512_t;\n\n";
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

  for (const auto &[Name, Identifier] : SourceRuntimeDataIdentifiers) {
    OS << "extern unsigned char " << Identifier << "[] __asm__(\"";
    OS.write_escaped("_" + Name);
    OS << "\");\n";
  }
  if (NeedsObjCSuper2)
    OS << "extern void objc_msgSendSuper2(void);\n";
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
    const auto ReturnType =
        InferredVoid ? NdType::makeVoid() : Function.ReturnType;
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
      Declarator += typeToC(Function.Params[I].Type);
    }
    if (ParamCount == 0)
      Declarator += "void";
    OS << declarationToC(ReturnType, Declarator + ")") << ";\n";
  }
  CurrentFunc = nullptr;
  Analysis = {};
  for (const auto *Function : SourceAddressDefinitions) {
    if (!Prototyped.insert(Function).second)
      continue;
    std::string Declarator = functionIdentifier(*Function) + "(";
    const size_t ParamCount = emittedParamCount(*Function);
    for (size_t I = 0; I < ParamCount; ++I) {
      if (I)
        Declarator += ", ";
      Declarator += typeToC(Function->Params[I].Type);
    }
    if (ParamCount == 0)
      Declarator += "void";
    if (Function->SourceTypeHint)
      OS << sourceConventionAttribute(Function->SourceTypeHint->Convention);
    OS << declarationToC(Function->ReturnType, Declarator + ")") << ";\n";
  }

  for (auto &Name : CallTargets) {
    if ((DefinedFuncs.count(Name) ||
         DefinedFunctionsByIdentifier.count(Name)) &&
        !SourceRuntimeLinkNames.count(Name))
      continue;
    if (libc::isKnownFunction(Name) && !isOwnFunctionName(Name, Funcs))
      continue;
    if (CIntrinsicNames.count(Name))
      continue;

    std::string CleanName = Name;
    if (!CleanName.empty() && CleanName[0] == '_')
      CleanName = CleanName.substr(1);
    if (libc::isKnownFunction(CleanName) && !isOwnFunctionName(Name, Funcs))
      continue;

    ExternFuncs.insert(Name);
  }

  for (const std::string &Name : ExternFuncs) {
    llvm::StringRef RenderedName(Name);
    RenderedName.consume_front("_");
    const auto Existing = ExternalFunctionIdentifiers.find(Name);
    std::string Identifier =
        Existing == ExternalFunctionIdentifiers.end()
            ? GlobalIdentifierAllocator.allocate(RenderedName, "nd_external")
            : Existing->second;
    ExternalFunctionIdentifiers.emplace(Name, Identifier);
    ExternalFunctionIdentifiers.try_emplace(RenderedName.str(), Identifier);
    auto SourceSignature = SourceNativeSignatures.find(Name);
    if (SourceSignature != SourceNativeSignatures.end() &&
        !ConflictingSourceNativeSignatures.count(Name)) {
      const auto &Declaration = SourceSignature->second;
      const auto &Signature = *Declaration.Signature;
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
    } else if (!ConflictingSourceNativeSignatures.count(Name)) {
      OS << "extern int " << Identifier << "()";
      if (libc::isNoReturnFunction(Name) ||
          libc::isNoReturnFunction(Identifier))
        OS << " __attribute__((noreturn))";
      OS << ";\n";
    }
  }

  if (!ExternFuncs.empty())
    OS << "\n";
}

void HighCWriter::collectImageObjects(const std::vector<HighFunc> &Funcs) {
  ImageObjects.clear();
  if (!Opts.Image)
    return;
  std::function<void(const HighExpr &)> Visit = [&](const HighExpr &E) {
    if (E.Kind == ExprKind::Load && !E.Operands.empty() && E.Operands[0]) {
      if (auto VA = constAddress(*E.Operands[0])) {
        const uint16_t Size = E.Type ? E.Type->Size : 0;
        if (!foldReadonlyScalar(*VA, Size))
          noteImageObject(*VA, E.Type, false);
      }
    }
    if (E.Kind == ExprKind::Store && E.Operands.size() >= 2 && E.Operands[0]) {
      if (auto VA = constAddress(*E.Operands[0]))
        noteImageObject(*VA, E.Operands[1] ? E.Operands[1]->Type : nullptr,
                        true);
    }
    if (E.Kind == ExprKind::Addr && !E.Operands.empty() && E.Operands[0] &&
        E.Operands[0]->Kind == ExprKind::Load &&
        !E.Operands[0]->Operands.empty() && E.Operands[0]->Operands[0]) {
      if (auto VA = constAddress(*E.Operands[0]->Operands[0]))
        noteImageObject(*VA, E.Operands[0]->Type, false);
    }
    for (const ExprPtr &Op : E.Operands)
      if (Op)
        Visit(*Op);
  };
  for (const HighFunc &Func : Funcs)
    walkStmts(Func.Body, [&](const HighStmt &S) {
      if (S.Kind == StmtKind::Store && S.StoreAddr) {
        if (auto VA = constAddress(*S.StoreAddr))
          noteImageObject(*VA, S.StoreVal ? S.StoreVal->Type : nullptr, true);
      }
      forEachExpr(S, [&](const ExprPtr &E) {
        if (E)
          Visit(*E);
      });
    });
}

void HighCWriter::writeImageObjects() {
  if (ImageObjects.empty())
    return;
  for (auto &[Addr, Obj] : ImageObjects) {
    if (Obj.Name.empty())
      Obj.Name = GlobalIdentifierAllocator.allocate(
          makeSyntheticGlobalName(Addr), "g");
    if (Opts.EmitComments)
      OS << "/* neverd.image: 0x" << llvm::utohexstr(Addr) << " */\n";
    // Tentative definition so standalone HighC can link.  LLVMC keeps
    // `extern` because it projects LLVM `external global`.
    OS << declarationToC(Obj.Type, Obj.Name) << ";\n";
  }
  OS << "\n";
}

void HighCWriter::writeAll(const std::vector<HighFunc> &Funcs) {
  prepareFunctionIdentifiers(Funcs);
  collectMemoryTypes(Funcs);
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
  writeMemoryHelpers();
  writeForwardDecls(Funcs);
  collectImageObjects(Funcs);
  writeImageObjects();

  for (size_t I = 0; I < Funcs.size(); ++I) {
    if (Funcs[I].Name.empty())
      continue;
    writeFunction(Funcs[I]);
    if (I + 1 < Funcs.size())
      OS << "\n";
  }
}

//===----------------------------------------------------------------------===//
// Public API
//===----------------------------------------------------------------------===//

bool HighCEmitter::emit(const std::vector<HighFunc> &Funcs,
                        llvm::raw_ostream &Out, const CEmitterOptions &Opts,
                        DebugContext *Dbg) {
  std::vector<HighFunc> Working = Funcs;
  attachCxxFuncletBodies(Working);
  HighCWriter W(Out, Opts, Dbg);
  W.writeAll(Working);
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
