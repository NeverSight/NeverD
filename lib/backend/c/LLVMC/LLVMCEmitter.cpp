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

#include "neverd/Common.h"
#include "neverd/backend/llvm/LLVMX86AddressSpaces.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Scalar/DCE.h"
#include "llvm/Transforms/Scalar/Scalarizer.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/LoopUtils.h"

#define DEBUG_TYPE "neverd-llvmc-emitter"

#include <cctype>
#include <stdexcept>

namespace neverd {

void LLVMCWriter::prepareFunctionIdentifiers(llvm::Module &Mod) {
  GlobalIdentifierAllocator = CProjectionIdentifierAllocator{};
  FunctionIdentifiers.clear();
  for (llvm::Function &Fn : Mod) {
    llvm::StringRef Name = Fn.getName();
    std::string DebugName;
    if (isSynthesizedFuncName(Name)) {
      llvm::StringRef Hex = Name;
      if (!Hex.consume_front(kAutoFuncPrefix))
        Hex.consume_front(kAutoFuncPrefixEVM);
      uint64_t Addr = 0;
      if (!Hex.empty() && !Hex.getAsInteger(16, Addr) && Addr != 0) {
        if (Dbg) {
          if (auto FS = Dbg->resolveFunction(static_cast<va_t>(Addr));
              FS && !FS->Name.empty())
            DebugName = FS->Name;
        }
        if (DebugName.empty() && Img) {
          std::string FromImage =
              Img->getFunctionNameAt(static_cast<va_t>(Addr));
          if (!FromImage.empty() && !isSynthesizedFuncName(FromImage))
            DebugName = std::move(FromImage);
          if (DebugName.empty())
            if (const Import *Imp = Img->findImportStubAt(Addr);
                Imp && !Imp->Name.empty())
              DebugName = Imp->Name;
        }
      }
    }
    if (!DebugName.empty())
      Name = DebugName;
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

void LLVMCWriter::writeModule(llvm::Module &Mod, const llvm::Function *Only) {
  OnlyFunction = Only;
  CurMod = &Mod;
  prepareFunctionIdentifiers(Mod);
  writeIncludes(Mod);
  OS << "\n";
  if (!OnlyFunction) {
    writeStructDefs(Mod);
    writeGlobals(Mod);
    writeForwardDecls(Mod);
    OS << "\n";
  } else {
    writeReferencedImageObjects(*OnlyFunction);
  }

  for (auto &Fn : Mod) {
    if (Fn.isDeclaration())
      continue;
    if (OnlyFunction && &Fn != OnlyFunction)
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
    if (OnlyFunction && &Fn != OnlyFunction)
      continue;
    if (!OnlyFunction && !Fn.isDeclaration() && GuardAnalysisOnlyFunctions &&
        isAnalysisOnlyFunction(Fn))
      continue;
    for (auto &BB : Fn) {
      for (auto &Inst : BB) {
        if (llvm::isa<llvm::FenceInst>(&Inst))
          HasCIntrinsics = true;
        if (auto *LI = llvm::dyn_cast<llvm::LoadInst>(&Inst)) {
          if (isLLVMX86SegmentedAddressSpace(LI->getPointerAddressSpace()))
            HasCIntrinsics = true;
        }
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
          if (isX86FastFailName(Name))
            HasCIntrinsics = true;

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
  // Permission to read a scalar from the image is not permission to remove a
  // volatile/atomic access. Its named object must survive declaration pruning.
  std::set<va_t> ObservedImageObjects;
  for (const auto &Fn : Mod)
    for (const auto &BB : Fn)
      for (const auto &Inst : BB) {
        const llvm::Value *Ptr = nullptr;
        if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Inst);
            Load && !Load->isSimple())
          Ptr = Load->getPointerOperand();
        if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Inst))
          Ptr = Store->getPointerOperand();
        if (Ptr)
          if (auto VA = imageDataVA(Ptr))
            ObservedImageObjects.insert(*VA);
      }
  for (auto &GV : Mod.globals()) {
    std::string RawName = GV.getName().str();
    if (RawName.empty())
      continue;

    // IAT/import slots are callable names at call sites, not C objects.
    // Printing `__imp_??1?$CStringT@...` as `extern uint64_t` is not C.
    if (!GV.hasInitializer()) {
      llvm::StringRef Raw = RawName;
      std::string ImportName;
      if (Raw.starts_with("__imp_") || Raw.starts_with("_imp_") ||
          Raw.starts_with("??") || Raw.starts_with("ord_"))
        ImportName = canonicalizeCProjectionIdentifier(Raw, "nd_import");
      else if (auto Slot = parseNdCodePtrSymbol(Raw)) {
        if (Img)
          if (const Import *Imp = Img->findImportAt(*Slot);
              Imp && !Imp->Name.empty())
            ImportName =
                canonicalizeCProjectionIdentifier(Imp->Name, "nd_import");
      } else if (auto Slot = parseNdDataSymbol(Raw)) {
        if (Img)
          if (const Import *Imp = Img->findImportAt(*Slot);
              Imp && !Imp->Name.empty())
            ImportName =
                canonicalizeCProjectionIdentifier(Imp->Name, "nd_import");
      }
      if (!ImportName.empty())
        continue;
    }

    std::string Name = resolveNdDataName(RawName);
    if (Name.empty()) {
      std::string Identifier = RawName;
      if (Identifier[0] == '_')
        Identifier.erase(0, 1);
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
    } else if (!GV.hasInitializer()) {
      llvm::StringRef Resolved = Name;
      if (Resolved.starts_with("__imp_") || Resolved.starts_with("_imp_") ||
          Resolved.starts_with("??") || Resolved.starts_with("ord_") ||
          Resolved.contains("??"))
        continue;
      if (auto Slot = parseNdDataSymbol(RawName); Slot && Img) {
        if (Img->findImportAt(*Slot))
          continue;
        auto *VTy = GV.getValueType();
        const uint16_t Size =
            VTy && VTy->isIntegerTy()
                ? static_cast<uint16_t>(VTy->getIntegerBitWidth() / 8)
                : 0;
        if (!ObservedImageObjects.count(*Slot) &&
            foldReadonlyScalar(*Slot, Size))
          continue;
      }
      auto *VTy = GV.getValueType();
      const char *CTy = "uint8_t";
      if (VTy->isIntegerTy(16))
        CTy = "uint16_t";
      else if (VTy->isIntegerTy(32))
        CTy = "uint32_t";
      else if (VTy->isIntegerTy(64))
        CTy = "uint64_t";
      OS << "extern " << CTy << " " << Name << "; /* 0x"
         << llvm::utohexstr(*parseNdDataSymbol(RawName)) << " */\n";
      continue;
    }

    if (!GV.hasInitializer())
      continue;

    auto *Init = GV.getInitializer();

    // Lifted writable image data can be one byte array with overlapping
    // integer views.  Keep the backing range as an array; printing it as a
    // pointer, or printing each offset as a separate C object, loses those
    // overlaps (including the two ten-byte x87 operands in FPREM samples).
    if (const auto *Array = llvm::dyn_cast<llvm::ArrayType>(GV.getValueType());
        parseNdDataSymbol(RawName) && Array &&
        Array->getElementType()->isIntegerTy(8) &&
        llvm::isa<llvm::ConstantAggregateZero>(Init)) {
      if (GV.hasLocalLinkage())
        OS << "static ";
      if (GV.isConstant())
        OS << "const ";
      OS << "uint8_t " << Name << "[" << Array->getNumElements()
         << "] = {0};\n";
      continue;
    }

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

void LLVMCWriter::writeReferencedImageObjects(const llvm::Function &Fn) {
  std::map<std::string, llvm::Type *> Objs;
  std::map<va_t, const llvm::GlobalVariable *> ByteArrays;
  auto Note = [&](const llvm::Value *Ptr, llvm::Type *Ty, bool MayFold) {
    if (!Ty)
      return;
    const uint64_t Size = imageIntegerAccessSize(Ty);
    if (auto Backing = imageByteArrayBacking(Ptr, Size)) {
      ByteArrays.emplace(*parseNdDataSymbol(Backing->first->getName()),
                         Backing->first);
      return;
    }
    std::string Name = imageDataCName(Ptr);
    if (Name.empty())
      return;
    if (auto VA = imageDataVA(Ptr)) {
      if (MayFold && foldReadonlyScalar(*VA, static_cast<uint16_t>(Size)))
        return;
    }
    llvm::StringRef Raw = Name;
    if (Raw.starts_with("__imp_") || Raw.starts_with("_imp_") ||
        Raw.starts_with("??") || Raw.starts_with("ord_") ||
        Raw.contains("??"))
      return;
    auto It = Objs.find(Name);
    if (It == Objs.end() ||
        (Ty->isIntegerTy() && It->second->isIntegerTy() &&
         Ty->getIntegerBitWidth() > It->second->getIntegerBitWidth()))
      Objs[Name] = Ty;
  };
  for (const auto &BB : Fn) {
    for (const auto &Inst : BB) {
      if (auto *LI = llvm::dyn_cast<llvm::LoadInst>(&Inst))
        Note(LI->getPointerOperand(), LI->getType(), LI->isSimple());
      else if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst))
        Note(SI->getPointerOperand(), SI->getValueOperand()->getType(), false);
    }
  }
  for (const auto &[Name, Ty] : Objs) {
    OS << "extern " << typeToCLLVM(Ty) << " " << Name << ";\n";
  }
  for (const auto &[Base, GV] : ByteArrays) {
    const auto *Array = llvm::cast<llvm::ArrayType>(GV->getValueType());
    if (GV->hasLocalLinkage())
      OS << "static ";
    if (GV->isConstant())
      OS << "const ";
    OS << "uint8_t " << namedImageObject(Base) << "["
       << Array->getNumElements() << "] = {0};\n";
  }
  if (!Objs.empty() || !ByteArrays.empty())
    OS << "\n";
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
    if (isX86FastFailName(RawName))
      continue;

    std::string Name = functionIdentifier(Fn);

    if (libc::isKnownFunction(Name))
      continue;
    if (const MsvcAtlCallee *Atl = msvcAtlCallee(Name)) {
      OS << msvcAtlSyntheticPrototype(Name, *Atl,
                                      Opts.TheArch == Arch::X64 && Atl->FastCall)
         << ";\n";
      continue;
    }

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

static void lowerCIntegerReductions(llvm::Module &Mod) {
  llvm::SmallVector<llvm::IntrinsicInst *> Work;
  for (auto &F : Mod)
    for (auto &BB : F)
      for (auto &I : BB)
        if (auto *Call = llvm::dyn_cast<llvm::IntrinsicInst>(&I))
          switch (Call->getIntrinsicID()) {
          case llvm::Intrinsic::vector_reduce_add:
          case llvm::Intrinsic::vector_reduce_mul:
          case llvm::Intrinsic::vector_reduce_and:
          case llvm::Intrinsic::vector_reduce_or:
          case llvm::Intrinsic::vector_reduce_xor:
            if (llvm::isa<llvm::FixedVectorType>(
                    Call->getArgOperand(0)->getType()))
              Work.push_back(Call);
            break;
          default:
            break;
          }
  for (auto *Call : Work) {
    llvm::IRBuilder<> B(Call);
    auto Id = Call->getIntrinsicID();
    auto *Vector = Call->getArgOperand(0);
    auto *Identity = llvm::getReductionIdentity(
        Id, Vector->getType()->getScalarType(), llvm::FastMathFlags{});
    auto *Result = llvm::getOrderedReduction(
        B, Identity, Vector, llvm::getArithmeticReductionInstruction(Id));
    Call->replaceAllUsesWith(Result);
    Call->eraseFromParent();
  }
}

// Scalarizer handles vector-to-vector casts, but not packing a vector into a
// scalar integer. Express the pack explicitly before scalarization so the C
// writer never sees an unassigned gather temporary.
static llvm::Value *balancedOr(llvm::IRBuilder<> &B,
                               llvm::SmallVector<llvm::Value *> Values) {
  while (Values.size() > 1) {
    llvm::SmallVector<llvm::Value *> Next;
    for (size_t I = 0; I < Values.size(); I += 2)
      Next.push_back(I + 1 < Values.size()
                         ? B.CreateOr(Values[I], Values[I + 1])
                         : Values[I]);
    Values = std::move(Next);
  }
  return Values.front();
}

static void lowerIntegerVectorBitcasts(llvm::Module &Mod) {
  llvm::SmallVector<llvm::BitCastInst *> Work;
  for (auto &F : Mod)
    for (auto &BB : F)
      for (auto &I : BB)
        if (auto *Cast = llvm::dyn_cast<llvm::BitCastInst>(&I))
          Work.push_back(Cast);
  for (auto *Cast : Work) {
    auto *Vector = llvm::dyn_cast<llvm::FixedVectorType>(Cast->getSrcTy());
    auto *Integer = llvm::dyn_cast<llvm::IntegerType>(Cast->getDestTy());
    bool Pack = Vector && Integer;
    if (!Pack) {
      Vector = llvm::dyn_cast<llvm::FixedVectorType>(Cast->getDestTy());
      Integer = llvm::dyn_cast<llvm::IntegerType>(Cast->getSrcTy());
    }
    if (!Vector || !Integer || !Vector->getElementType()->isIntegerTy())
      continue;
    unsigned Count = Vector->getNumElements();
    unsigned Bits = Vector->getElementType()->getIntegerBitWidth();
    if (uint64_t(Count) * Bits != Integer->getBitWidth())
      continue;
    llvm::IRBuilder<> B(Cast);
    llvm::SmallVector<llvm::Value *> Parts;
    llvm::Value *Value =
        Pack ? static_cast<llvm::Value *>(llvm::ConstantInt::get(Integer, 0))
             : llvm::PoisonValue::get(Vector);
    for (unsigned Lane = 0; Lane < Count; ++Lane) {
      unsigned Shift =
          (Mod.getDataLayout().isLittleEndian() ? Lane : Count - Lane - 1) *
          Bits;
      if (Pack) {
        auto *Part = B.CreateZExtOrTrunc(
            B.CreateExtractElement(Cast->getOperand(0), Lane), Integer);
        if (Shift)
          Part = B.CreateShl(Part, llvm::ConstantInt::get(Integer, Shift));
        Parts.push_back(Part);
      } else {
        auto *Part = Cast->getOperand(0);
        if (Shift)
          Part = B.CreateLShr(Part, llvm::ConstantInt::get(Integer, Shift));
        Part = B.CreateZExtOrTrunc(Part, Vector->getElementType());
        Value = B.CreateInsertElement(Value, Part, Lane);
      }
    }
    if (Pack)
      Value = balancedOr(B, std::move(Parts));
    Cast->replaceAllUsesWith(Value);
    Cast->eraseFromParent();
  }
}

static bool containsVectorType(llvm::Type *Type,
                               llvm::SmallPtrSetImpl<llvm::Type *> &Seen) {
  if (!Seen.insert(Type).second)
    return false;
  if (Type->isVectorTy())
    return true;
  if (auto *Array = llvm::dyn_cast<llvm::ArrayType>(Type))
    return containsVectorType(Array->getElementType(), Seen);
  if (auto *Struct = llvm::dyn_cast<llvm::StructType>(Type)) {
    if (Struct->isOpaque())
      return false;
    for (auto *Element : Struct->elements())
      if (containsVectorType(Element, Seen))
        return true;
  }
  if (auto *Function = llvm::dyn_cast<llvm::FunctionType>(Type)) {
    if (containsVectorType(Function->getReturnType(), Seen))
      return true;
    for (auto *Parameter : Function->params())
      if (containsVectorType(Parameter, Seen))
        return true;
  }
  return false;
}

static bool containsVectorType(llvm::Type *Type) {
  llvm::SmallPtrSet<llvm::Type *, 16> Seen;
  return containsVectorType(Type, Seen);
}

bool LLVMCEmitter::emit(llvm::Module &Mod, llvm::raw_ostream &Out,
                        const CEmitterOptions &Opts, DebugContext *Dbg,
                        const BinaryImage *Img, const llvm::Function *Only) {
  bool HasVectors = false;
  if (!Only)
    for (const auto &Global : Mod.globals())
      HasVectors |= containsVectorType(Global.getValueType());
  for (const auto &Function : Mod) {
    if (Only && &Function != Only)
      continue;
    HasVectors |= containsVectorType(Function.getFunctionType());
    for (const auto &Block : Function)
      for (const auto &Instruction : Block) {
        HasVectors |= Instruction.getType()->isVectorTy();
        for (const auto &Operand : Instruction.operands())
          HasVectors |= Operand->getType()->isVectorTy();
      }
  }
  std::unique_ptr<llvm::Module> Projection;
  const llvm::Function *ProjectionOnly = Only;
  if (HasVectors) {
    // Normalize vector operations on a clone so C emission preserves the
    // caller's IR while the scalar writer receives explicit lane semantics.
    llvm::ValueToValueMapTy ValueMap;
    Projection = llvm::CloneModule(Mod, ValueMap);
    if (Only) {
      ProjectionOnly =
          llvm::dyn_cast_or_null<llvm::Function>(ValueMap.lookup(Only));
      if (!ProjectionOnly)
        throw std::runtime_error(
            "C projection could not map the selected function into its clone");
    }
    lowerCIntegerReductions(*Projection);
    lowerIntegerVectorBitcasts(*Projection);
    llvm::LoopAnalysisManager Loops;
    llvm::FunctionAnalysisManager Functions;
    llvm::CGSCCAnalysisManager CallGraph;
    llvm::ModuleAnalysisManager Modules;
    llvm::PassBuilder Passes;
    Passes.registerModuleAnalyses(Modules);
    Passes.registerCGSCCAnalyses(CallGraph);
    Passes.registerFunctionAnalyses(Functions);
    Passes.registerLoopAnalyses(Loops);
    Passes.crossRegisterProxies(Loops, Functions, CallGraph, Modules);
    llvm::FunctionPassManager Normalize;
    Normalize.addPass(llvm::ScalarizerPass());
    Normalize.addPass(llvm::DCEPass());
    llvm::ModulePassManager Pipeline;
    Pipeline.addPass(
        llvm::createModuleToFunctionPassAdaptor(std::move(Normalize)));
    Pipeline.run(*Projection, Modules);
    if (llvm::verifyModule(*Projection, &llvm::errs()))
      return false;
    if (!ProjectionOnly)
      for (const auto &Global : Projection->globals())
        if (containsVectorType(Global.getValueType()))
          throw std::runtime_error(
              "C projection retains an unsupported vector global");
    for (const auto &F : *Projection) {
      if (ProjectionOnly && &F != ProjectionOnly)
        continue;
      if (!ProjectionOnly && F.isDeclaration() && F.isIntrinsic())
        continue;
      if (containsVectorType(F.getFunctionType()))
        throw std::runtime_error(
            "C projection retains an unsupported vector signature");
      for (const auto &BB : F)
        for (const auto &I : BB)
          if (containsVectorType(I.getType()) ||
              llvm::any_of(I.operands(), [](const llvm::Use &Operand) {
                return containsVectorType(Operand->getType());
              }))
            throw std::runtime_error(
                "C projection retains an unsupported vector instruction");
    }
  }
  LLVMCWriter W(Out, Opts, Dbg, Img);
  W.writeModule(Projection ? *Projection : Mod,
                Projection ? ProjectionOnly : Only);
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
