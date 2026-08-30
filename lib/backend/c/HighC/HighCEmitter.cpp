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
#include "neverd/libc/LibCNames.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <functional>
#include <map>
#include <set>

namespace neverd {

namespace {

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
  switch (AddressSpace) {
  case NdMemoryAddressSpace::X86FS:
    return 257;
  case NdMemoryAddressSpace::X86GS:
    return 256;
  case NdMemoryAddressSpace::Default:
    break;
  }
  llvm::report_fatal_error(
      "default memory does not have a target address-space attribute");
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

  for (const HighFunc &Func : Funcs) {
    if (Func.Name.empty())
      continue;
    llvm::StringRef SourceName(Func.Name);
    llvm::StringRef RenderedName = SourceName;
    RenderedName.consume_front("_");
    std::string Identifier =
        GlobalIdentifierAllocator.allocate(RenderedName, "nd_function");
    FunctionIdentifiers.emplace(&Func, Identifier);
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
  SegmentedMemoryTypes.clear();
  AtomicLoadTypes.clear();
  AtomicStoreTypes.clear();
  HasSegmentedMemory = false;
  Has256BitInteger = false;
  auto CollectWideType = [&](const TypeRef &Type) {
    Has256BitInteger |= Type && Type->Kind == NdTypeKind::Int &&
                        Type->Size == 32;
  };
  std::set<const HighExpr *> Seen;
  std::function<void(const HighExpr &)> Visit = [&](const HighExpr &E) {
    if (!Seen.insert(&E).second)
      return;
    CollectWideType(E.Type);
    if (E.MemoryAddressSpace != NdMemoryAddressSpace::Default) {
      validateMemoryAddressSpaceForC(E.MemoryAddressSpace, Opts.TheArch);
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
      Names.insert(Type);
      validateMemoryAddressSpaceForC(E.MemoryAddressSpace, Opts.TheArch);
      if (E.MemoryAddressSpace != NdMemoryAddressSpace::Default)
        SegmentedMemoryTypes.insert({Type, E.MemoryAddressSpace});
      if (E.MemoryOrdering != NdMemoryOrdering::None)
        AtomicLoadTypes.insert({Type, E.MemoryOrdering, E.MemoryAddressSpace});
    }
    if (E.Kind == ExprKind::Store && E.Operands.size() >= 2) {
      std::string Type = memoryTypeName(E.Operands[1]->Type);
      Names.insert(Type);
      validateMemoryAddressSpaceForC(E.MemoryAddressSpace, Opts.TheArch);
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
    if (GuardAnalysisOnlyFunctions && isAnalysisOnlyFunction(Func))
      continue;
    CollectWideType(Func.ReturnType);
    for (const HighParam &Param : Func.Params)
      CollectWideType(Param.Type);
    for (const HighLocal &Local : Func.Locals)
      CollectWideType(Local.Type);
    walkStmts(Func.Body, [&](const HighStmt &Stmt) {
      if (Stmt.MemoryAddressSpace != NdMemoryAddressSpace::Default) {
        validateMemoryAddressSpaceForC(Stmt.MemoryAddressSpace, Opts.TheArch);
        HasSegmentedMemory = true;
        if (Stmt.Kind != StmtKind::Store)
          llvm::report_fatal_error(
              "HighIR address space is attached to a non-memory statement");
      }
      if (Stmt.Kind == StmtKind::Store && Stmt.StoreVal) {
        std::string Type = memoryTypeName(Stmt.StoreVal->Type);
        Names.insert(Type);
        validateMemoryAddressSpaceForC(Stmt.MemoryAddressSpace, Opts.TheArch);
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

  for (const auto &[Type, Index] : MemoryTypes) {
    OS << "static inline " << Type << " neverd_mem_load_" << Index
       << "(uintptr_t address) {\n"
       << "    " << Type << " value;\n"
       << "    memcpy(&value, (const void *)address, sizeof(value));\n"
       << "    return value;\n"
       << "}\n\n"
       << "static inline " << Type << " neverd_mem_store_" << Index
       << "(uintptr_t address, " << Type << " value) {\n"
       << "    memcpy((void *)address, &value, sizeof(value));\n"
       << "    return value;\n"
       << "}\n\n";
  }

  for (const auto &[Type, AddressSpace] : SegmentedMemoryTypes) {
    validateMemoryAddressSpaceForC(AddressSpace, Opts.TheArch);
    const unsigned Index = MemoryTypes.at(Type);
    const std::string LoadName =
        memoryHelperName("load", Index, NdMemoryOrdering::None, AddressSpace);
    const std::string StoreName =
        memoryHelperName("store", Index, NdMemoryOrdering::None, AddressSpace);
    const std::string ReadPtr =
        memoryPointerCast(Type, "address", AddressSpace, true);
    const std::string WritePtr =
        memoryPointerCast(Type, "address", AddressSpace, false);
    OS << "static inline " << Type << " " << LoadName
       << "(uintptr_t address) {\n"
       << "    " << Type << " value;\n"
       << "    __builtin_memcpy(&value, " << ReadPtr << ", sizeof(value));\n"
       << "    return value;\n"
       << "}\n\n"
       << "static inline " << Type << " " << StoreName << "(uintptr_t address, "
       << Type << " value) {\n"
       << "    __builtin_memcpy(" << WritePtr << ", &value, sizeof(value));\n"
       << "    return value;\n"
       << "}\n\n";
  }

  for (const auto &[Type, Ordering, AddressSpace] : AtomicLoadTypes) {
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
  auto It = MemoryTypes.find(Type);
  if (It == MemoryTypes.end())
    llvm::report_fatal_error("HighC memory store type was not collected");
  unsigned Index = It->second;
  if (Ordering != NdMemoryOrdering::None)
    validateAtomicStoreOrdering(Ordering);
  return memoryHelperName("store", Index, Ordering, AddressSpace) +
         "((uintptr_t)(" + Addr.str() + "), " + Val.str() + ")";
}

std::string
HighCWriter::atomicExchangeExpr(const TypeRef &Ty, llvm::StringRef Addr,
                                llvm::StringRef Val, NdMemoryOrdering Ordering,
                                NdMemoryAddressSpace AddressSpace) const {
  if (Ordering == NdMemoryOrdering::None)
    llvm::report_fatal_error("atomic exchange requires memory ordering");
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
      if (Ex.IntrinsicId == Intrinsic::A64_Frinti)
        NeedsFEnvAccess = true;
      std::string Name = Ex.CallTarget;
      if (!Name.empty()) {
        if (Ex.IntrinsicId != Intrinsic::None) {
          if (intrinsicCName(Ex.IntrinsicId))
            HasCIntrinsics = true;
          CIntrinsicNames.insert(Name);
        }
        if (Ex.IntrinsicId == Intrinsic::None && Name[0] == '_')
          Name = Name.substr(1);
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

void HighCWriter::writeIncludes(const std::vector<HighFunc> &Funcs) {
  if (!Opts.EmitIncludes) {
    if (Has256BitInteger)
      OS << "typedef unsigned _BitInt(256) uint256_t;\n"
            "typedef _BitInt(256) int256_t;\n\n";
    return;
  }

  std::set<std::string> Headers;
  Headers.insert("stdint.h");
  if (!MemoryTypes.empty())
    Headers.insert("string.h");

  std::set<std::string> CallTargets;
  for (auto &F : Funcs) {
    if (GuardAnalysisOnlyFunctions && isAnalysisOnlyFunction(F))
      continue;
    collectCallTargets(F.Body, CallTargets);
  }

  for (auto &Name : CallTargets) {
    if (const char *Hdr = libc::headerFor(Name))
      Headers.insert(Hdr);
  }

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
}

void HighCWriter::writeForwardDecls(const std::vector<HighFunc> &Funcs) {
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
  for (auto &F : Funcs) {
    if (GuardAnalysisOnlyFunctions && isAnalysisOnlyFunction(F))
      continue;
    collectCallTargets(F.Body, CallTargets);
  }

  for (auto &Name : CallTargets) {
    if (DefinedFuncs.count(Name))
      continue;
    if (libc::isKnownFunction(Name))
      continue;
    if (CIntrinsicNames.count(Name))
      continue;

    std::string CleanName = Name;
    if (!CleanName.empty() && CleanName[0] == '_')
      CleanName = CleanName.substr(1);
    if (libc::isKnownFunction(CleanName))
      continue;

    ExternFuncs.insert(Name);
  }

  for (const std::string &Name : ExternFuncs) {
    llvm::StringRef RenderedName(Name);
    RenderedName.consume_front("_");
    std::string Identifier =
        GlobalIdentifierAllocator.allocate(RenderedName, "nd_external");
    ExternalFunctionIdentifiers.emplace(Name, Identifier);
    ExternalFunctionIdentifiers.try_emplace(RenderedName.str(), Identifier);
    OS << "extern int " << Identifier << "();\n";
  }

  if (!ExternFuncs.empty())
    OS << "\n";
}

void HighCWriter::writeAll(const std::vector<HighFunc> &Funcs) {
  prepareFunctionIdentifiers(Funcs);
  collectMemoryTypes(Funcs);
  writeIncludes(Funcs);
  writeMemoryHelpers();
  writeForwardDecls(Funcs);

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
  HighCWriter W(Out, Opts, Dbg);
  W.writeAll(Funcs);
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
