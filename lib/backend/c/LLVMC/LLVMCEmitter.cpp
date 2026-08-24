//===- LLVMCEmitter.cpp - LLVM IR to C emitter ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Top-level orchestration for the LLVM-IR C emitter: include generation,
/// struct/global/forward-declaration emission, and per-function dispatch.
/// Function and instruction rendering live in LLVMCFuncWriter.cpp and
/// LLVMCStmtWriter.cpp; value/expression rendering lives in
/// LLVMCExprWriter.cpp.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"

#include "LLVMCWriter.h"

#define DEBUG_TYPE "neverd-llvmc-emitter"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <cctype>

namespace neverd {

void LLVMCWriter::prepareFunctionIdentifiers(llvm::Module &Mod) {
  GlobalIdentifierAllocator = CProjectionIdentifierAllocator{};
  FunctionIdentifiers.clear();
  for (llvm::Function &Fn : Mod) {
    llvm::StringRef Name = Fn.getName();
    Name.consume_front("_");
    FunctionIdentifiers.emplace(
        &Fn, GlobalIdentifierAllocator.allocate(Name, "nd_function"));
  }
}

std::string LLVMCWriter::functionIdentifier(const llvm::Function &Fn) const {
  if (auto It = FunctionIdentifiers.find(&Fn); It != FunctionIdentifiers.end())
    return It->second;
  llvm::StringRef Name = Fn.getName();
  Name.consume_front("_");
  return canonicalizeCProjectionIdentifier(Name, "nd_function");
}

void LLVMCWriter::writeModule(llvm::Module &Mod) {
  prepareFunctionIdentifiers(Mod);
  writeIncludes(Mod);
  OS << "\n";
  writeStructDefs(Mod);
  writeGlobals(Mod);
  writeForwardDecls(Mod);
  OS << "\n";

  for (auto &Fn : Mod) {
    if (Fn.isDeclaration())
      continue;
    writeFunction(Fn);
    OS << "\n";
  }
}

void LLVMCWriter::writeIncludes(llvm::Module &Mod) {
  if (!Opts.EmitIncludes)
    return;

  std::set<std::string> Headers;
  Headers.insert("stdint.h");

  for (auto &Fn : Mod) {
    if (!Fn.isDeclaration() && GuardAnalysisOnlyFunctions &&
        isAnalysisOnlyFunction(Fn))
      continue;
    for (auto &BB : Fn) {
      for (auto &Inst : BB) {
        if (llvm::isa<llvm::FenceInst>(&Inst))
          HasCIntrinsics = true;
        if (auto *CI = llvm::dyn_cast<llvm::CallInst>(&Inst)) {
          if (llvm::dyn_cast<llvm::InlineAsm>(CI->getCalledOperand()))
            continue;
          auto *Callee = CI->getCalledFunction();
          if (!Callee)
            continue;
          auto Name = Callee->getName().str();

          if (llvmIntrinsicToCName(Name.c_str())) {
            HasCIntrinsics = true;
            IntrinsicMappedNames.insert(Name);
          }

          if (const char *Hdr = libc::headerFor(stripLeadingUnderscores(Name)))
            Headers.insert(Hdr);
        }
      }
    }
  }

  if (HasCIntrinsics)
    for (const char *Hdr : getArchIntrinsicHeaders(Opts.TheArch))
      Headers.insert(Hdr);

  for (auto &H : Headers)
    OS << "#include <" << H << ">\n";
  OS << "\n";
}

void LLVMCWriter::writeStructDefs(llvm::Module &Mod) {
  std::set<llvm::StructType *> Seen;
  auto Structs = Mod.getIdentifiedStructTypes();
  for (auto *ST : Structs) {
    if (!Seen.insert(ST).second)
      continue;
    if (ST->isOpaque()) {
      OS << llvmStructName(ST) << ";\n";
      continue;
    }
    OS << llvmStructName(ST) << " {\n";
    for (unsigned I = 0; I < ST->getNumElements(); ++I) {
      OS << "    " << typeToCLLVM(ST->getElementType(I)) << " field_" << I
         << ";\n";
    }
    OS << "};\n\n";
  }
}

void LLVMCWriter::writeGlobals(llvm::Module &Mod) {
  for (auto &GV : Mod.globals()) {
    std::string RawName = GV.getName().str();
    if (RawName.empty())
      continue;

    if (!GV.hasInitializer()) {
      llvm::StringRef SRef(RawName);
      llvm::StringRef Hex;
      if (SRef.starts_with(kNdDataPrefix))
        Hex = SRef.drop_front(kNdDataPrefix.size());
      else if (SRef.starts_with("_") &&
               SRef.drop_front(1).starts_with(kNdDataPrefix))
        Hex = SRef.drop_front(1 + kNdDataPrefix.size());
      if (!Hex.empty()) {
        uint64_t Addr;
        if (!Hex.getAsInteger(16, Addr)) {
          std::string Resolved = resolveNdDataName(RawName);
          auto *VTy = GV.getValueType();
          const char *CTy = "uint8_t";
          if (VTy->isIntegerTy(16))
            CTy = "uint16_t";
          else if (VTy->isIntegerTy(32))
            CTy = "uint32_t";
          else if (VTy->isIntegerTy(64))
            CTy = "uint64_t";
          OS << "extern " << CTy << " " << Resolved << "; /* 0x"
             << llvm::utohexstr(Addr) << " */\n";
        }
      }
      continue;
    }

    std::string Identifier = RawName;
    if (Identifier[0] == '_')
      Identifier.erase(0, 1);
    std::string Name;
    for (char Ch : Identifier) {
      if (std::isalnum(static_cast<unsigned char>(Ch)) || Ch == '_')
        Name += Ch;
      else
        Name += '_';
    }
    if (Name.empty())
      Name = "g_";
    else if (std::isdigit(static_cast<unsigned char>(Name[0])))
      Name = "g_" + Name;

    auto *Init = GV.getInitializer();

    if (auto *CDA = llvm::dyn_cast<llvm::ConstantDataArray>(Init)) {
      if (CDA->isString()) {
        llvm::StringRef Raw = CDA->getAsString();
        while (!Raw.empty() && Raw.back() == '\0')
          Raw = Raw.drop_back();
        OS << "static const char " << Name << "[] = \"" << escapeCString(Raw)
           << "\";\n";
        continue;
      }
    }

    if (GV.isConstant())
      OS << "const ";
    OS << typeToCLLVM(GV.getValueType()) << " " << Name;
    if (!llvm::isa<llvm::ConstantAggregateZero>(Init))
      OS << " = " << constStr(Init);
    OS << ";\n";
  }
}

void LLVMCWriter::writeForwardDecls(llvm::Module &Mod) {
  for (auto &Fn : Mod) {
    if (!Fn.isDeclaration())
      continue;
    if (Fn.isIntrinsic())
      continue;
    if (GuardAnalysisOnlyFunctions && !isReferencedByExecutableProjection(Fn))
      continue;

    std::string RawName = Fn.getName().str();

    if (IntrinsicMappedNames.count(RawName))
      continue;

    if (llvmIntrinsicToCName(RawName.c_str()))
      continue;

    std::string Name = functionIdentifier(Fn);

    if (libc::isKnownFunction(Name))
      continue;

    auto *FT = Fn.getFunctionType();
    OS << typeToCLLVM(FT->getReturnType()) << " " << Name << "(";
    if (FT->getNumParams() == 0 && !FT->isVarArg()) {
      OS << "void";
    } else if (FT->getNumParams() == 0 && FT->isVarArg()) {
      // ISO C before C23 cannot spell a prototype containing only `...`.
      // An empty parameter list is deliberately non-prototyped and therefore
      // keeps the analysis projection callable without inventing an ABI-visible
      // fixed argument.
    } else {
      for (unsigned I = 0; I < FT->getNumParams(); ++I) {
        if (I > 0)
          OS << ", ";
        OS << typeToCLLVM(FT->getParamType(I));
      }
      if (FT->isVarArg())
        OS << ", ...";
    }
    OS << ");\n";
  }
}

//===----------------------------------------------------------------------===//
// Public API
//===----------------------------------------------------------------------===//

bool LLVMCEmitter::emit(llvm::Module &Mod, llvm::raw_ostream &Out,
                        const CEmitterOptions &Opts, DebugContext *Dbg,
                        const BinaryImage *Img) {
  LLVMCWriter W(Out, Opts, Dbg, Img);
  W.writeModule(Mod);
  return true;
}

bool LLVMCEmitter::emitToFile(llvm::Module &Mod, const std::string &Path,
                              const CEmitterOptions &Opts, DebugContext *Dbg,
                              const BinaryImage *Img) {
  std::error_code EC;
  llvm::raw_fd_ostream OS(Path, EC);
  if (EC) {
    llvm::WithColor::error() << "llvm_c_emitter: cannot open " << Path << ": "
                             << EC.message() << "\n";
    return false;
  }
  bool Ok = emit(Mod, OS, Opts, Dbg, Img);
  LLVM_DEBUG(llvm::dbgs() << "llvm_c_emitter: written to " << Path << "\n");
  return Ok;
}

} // namespace neverd
