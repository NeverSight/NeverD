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
#include "llvm/IR/IntrinsicsAArch64.h"
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

bool LLVMCWriter::isNativeVectorIntrinsic(const llvm::CallBase &Call,
                                          Arch TheArch) {
  const auto *Callee = Call.getCalledFunction();
  if (TheArch != Arch::AArch64 || !Callee ||
      Callee->getIntrinsicID() != llvm::Intrinsic::aarch64_neon_bfmmla ||
      Call.arg_size() != 3)
    return false;
  auto &Context = Call.getContext();
  auto *Accumulator =
      llvm::FixedVectorType::get(llvm::Type::getFloatTy(Context), 4);
  auto *Input = llvm::FixedVectorType::get(llvm::Type::getBFloatTy(Context), 8);
  return Call.getType() == Accumulator &&
         Call.getArgOperand(0)->getType() == Accumulator &&
         Call.getArgOperand(1)->getType() == Input &&
         Call.getArgOperand(2)->getType() == Input &&
         Call.getFunctionType() == Callee->getFunctionType();
}

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
    writeForwardDecls(Mod);
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
  std::set<std::string> Headers;
  Headers.insert("stdint.h");
  std::set<std::pair<unsigned, bool>> FunnelShifts;

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
          const auto IID = Callee->getIntrinsicID();
          if (IID == llvm::Intrinsic::fshl || IID == llvm::Intrinsic::fshr) {
            const auto *Ty = llvm::dyn_cast<llvm::IntegerType>(CI->getType());
            const unsigned Width = Ty ? Ty->getBitWidth() : 0;
            if (CI->arg_size() != 3 ||
                (Width != 8 && Width != 16 && Width != 32 && Width != 64 &&
                 Width != 128))
              throw std::runtime_error("unsupported LLVM funnel shift width");
            FunnelShifts.insert({Width, IID == llvm::Intrinsic::fshl});
          }
          auto Name = Callee->getName().str();

          if (const char *Mapped = llvmIntrinsicToCName(Name.c_str())) {
            HasCIntrinsics = true;
            IntrinsicMappedNames.insert(Name);
            if (const char *Header = libc::headerFor(Mapped))
              Headers.insert(Header);
          }
          if (isNativeVectorIntrinsic(*CI, Opts.TheArch))
            HasCIntrinsics = true;
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

  if (Opts.EmitIncludes) {
    for (auto &H : Headers)
      OS << "#include <" << H << ">\n";
    OS << "\n";
  }
  for (const auto &[Width, Left] : FunnelShifts) {
    const std::string Type = Width == 128
                                 ? "unsigned __int128"
                                 : "uint" + std::to_string(Width) + "_t";
    OS << "static inline " << Type << " neverd_llvm_fsh" << (Left ? "l" : "r")
       << "_i" << Width << "(" << Type << " a, " << Type << " b, " << Type
       << " amount) {\n"
       << "    unsigned int shift = (unsigned int)(amount % " << Width
       << ");\n";
    if (Left)
      OS << "    return shift == 0 ? a : (" << Type
         << ")((a << shift) | (b >> (" << Width << " - shift)));\n";
    else
      OS << "    return shift == 0 ? b : (" << Type << ")((a << (" << Width
         << " - shift)) | (b >> shift));\n";
    OS << "}\n\n";
  }
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
        Raw.starts_with("??") || Raw.starts_with("ord_") || Raw.contains("??"))
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
    OS << "uint8_t " << namedImageObject(Base) << "[" << Array->getNumElements()
       << "] = {0};\n";
  }
  if (!Objs.empty() || !ByteArrays.empty())
    OS << "\n";
}

void LLVMCWriter::writeForwardDecls(llvm::Module &Mod) {
  for (auto &Fn : Mod) {
    if (OnlyFunction) {
      // Vector declarations retain the calling convention in a selected
      // function fragment without expanding its scalar declaration surface.
      const auto *Signature = Fn.getFunctionType();
      const bool HasVectorSignature =
          Signature->getReturnType()->isVectorTy() ||
          llvm::any_of(Signature->params(), [](const llvm::Type *Type) {
            return Type->isVectorTy();
          });
      if (!HasVectorSignature || &Fn == OnlyFunction ||
          !llvm::any_of(Fn.users(), [&](const llvm::User *User) {
            const auto *Instruction = llvm::dyn_cast<llvm::Instruction>(User);
            return Instruction && Instruction->getFunction() == OnlyFunction;
          }))
        continue;
    } else if (!Fn.isDeclaration())
      continue;
    if (Fn.isIntrinsic())
      continue;
    if (!OnlyFunction && GuardAnalysisOnlyFunctions &&
        !isReferencedByExecutableProjection(Fn))
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
      OS << msvcAtlSyntheticPrototype(
                Name, *Atl, Opts.TheArch == Arch::X64 && Atl->FastCall)
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

static void lowerPackedVectorBitcasts(llvm::Module &Mod) {
  llvm::SmallVector<llvm::BitCastInst *> Work;
  for (auto &F : Mod)
    for (auto &BB : F)
      for (auto &I : BB)
        if (auto *Cast = llvm::dyn_cast<llvm::BitCastInst>(&I))
          Work.push_back(Cast);
  for (auto *Cast : Work) {
    auto *Vector = llvm::dyn_cast<llvm::FixedVectorType>(Cast->getSrcTy());
    const bool Pack = Vector != nullptr;
    auto *Scalar = Pack ? Cast->getDestTy() : Cast->getSrcTy();
    if (!Pack)
      Vector = llvm::dyn_cast<llvm::FixedVectorType>(Cast->getDestTy());
    auto IsLane = [](llvm::Type *Type) {
      return Type->isIntegerTy() || Type->isFloatTy() || Type->isDoubleTy() ||
             Type->isBFloatTy();
    };
    if (!Vector || !IsLane(Scalar) || !IsLane(Vector->getElementType()))
      continue;
    const unsigned ScalarBits = Scalar->getPrimitiveSizeInBits();
    const unsigned Count = Vector->getNumElements();
    const unsigned Bits = Vector->getElementType()->getPrimitiveSizeInBits();
    if (ScalarBits > 128 || uint64_t(Count) * Bits != ScalarBits)
      continue;
    auto *Integer = llvm::IntegerType::get(Mod.getContext(), ScalarBits);
    auto *LaneInteger = llvm::IntegerType::get(Mod.getContext(), Bits);
    llvm::IRBuilder<> B(Cast);
    auto *Input = Cast->getOperand(0);
    if (!Pack && !Scalar->isIntegerTy())
      Input = B.CreateBitCast(Input, Integer);
    llvm::SmallVector<llvm::Value *> Parts;
    llvm::Value *Value =
        Pack ? static_cast<llvm::Value *>(llvm::ConstantInt::get(Integer, 0))
             : llvm::PoisonValue::get(Vector);
    for (unsigned Lane = 0; Lane < Count; ++Lane) {
      unsigned Shift =
          (Mod.getDataLayout().isLittleEndian() ? Lane : Count - Lane - 1) *
          Bits;
      if (Pack) {
        auto *Part = B.CreateExtractElement(Input, Lane);
        if (!Part->getType()->isIntegerTy())
          Part = B.CreateBitCast(Part, LaneInteger);
        Part = B.CreateZExtOrTrunc(Part, Integer);
        if (Shift)
          Part = B.CreateShl(Part, llvm::ConstantInt::get(Integer, Shift));
        Parts.push_back(Part);
      } else {
        auto *Part = Input;
        if (Shift)
          Part = B.CreateLShr(Part, llvm::ConstantInt::get(Integer, Shift));
        Part = B.CreateZExtOrTrunc(Part, LaneInteger);
        if (!Vector->getElementType()->isIntegerTy())
          Part = B.CreateBitCast(Part, Vector->getElementType());
        Value = B.CreateInsertElement(Value, Part, Lane);
      }
    }
    if (Pack) {
      Value = balancedOr(B, std::move(Parts));
      if (!Scalar->isIntegerTy())
        Value = B.CreateBitCast(Value, Scalar);
    }
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

static bool referencesVectorGlobal(const llvm::Function &Function) {
  llvm::SmallVector<const llvm::Value *, 32> Work;
  llvm::SmallPtrSet<const llvm::Value *, 32> Seen;
  for (const auto &Block : Function)
    for (const auto &Instruction : Block)
      Work.push_back(&Instruction);
  while (!Work.empty()) {
    const auto *Value = Work.pop_back_val();
    if (!Seen.insert(Value).second || llvm::isa<llvm::Function>(Value))
      continue;
    if (const auto *Global = llvm::dyn_cast<llvm::GlobalVariable>(Value))
      if (containsVectorType(Global->getValueType()))
        return true;
    // Follow constant GEPs, casts, aliases and pointer-valued initializers as
    // well as direct uses; opaque pointers do not reveal the storage type.
    if (const auto *User = llvm::dyn_cast<llvm::User>(Value))
      for (const auto &Operand : User->operands())
        Work.push_back(Operand.get());
  }
  return false;
}

static bool isCVectorBoundaryType(llvm::Type *Type) {
  return isCIntegerVectorType(Type) || !containsVectorType(Type);
}

static bool isCVectorBoundarySignature(const llvm::FunctionType *Type) {
  return isCVectorBoundaryType(Type->getReturnType()) &&
         llvm::all_of(Type->params(), isCVectorBoundaryType);
}

static bool isCVectorBoundaryInstruction(const llvm::Instruction &Inst,
                                         Arch TheArch) {
  auto IsLocalType = [](llvm::Type *Type) {
    return isCVectorType(Type) || !containsVectorType(Type);
  };
  if (!IsLocalType(Inst.getType()) ||
      !llvm::all_of(Inst.operands(), [&](const llvm::Use &Operand) {
        return IsLocalType(Operand->getType());
      }))
    return false;
  // The scalarizer retains gathers/scatters at function boundaries. C vector
  // types preserve those lanes and their calling convention; arithmetic and
  // memory operations still need the existing explicit scalar lowering.
  if (llvm::isa<llvm::ExtractElementInst, llvm::InsertElementInst,
                llvm::ReturnInst, llvm::PHINode>(Inst))
    return true;
  if (const auto *Select = llvm::dyn_cast<llvm::SelectInst>(&Inst))
    return Select->getCondition()->getType()->isIntegerTy(1);
  if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Inst)) {
    const auto *Callee = Call->getCalledFunction();
    if (LLVMCWriter::isNativeVectorIntrinsic(*Call, TheArch))
      return true;
    return Callee && !Callee->isIntrinsic() &&
           isCVectorBoundarySignature(Call->getFunctionType());
  }
  return false;
}

bool LLVMCEmitter::emit(llvm::Module &Mod, llvm::raw_ostream &Out,
                        const CEmitterOptions &Opts, DebugContext *Dbg,
                        const BinaryImage *Img, const llvm::Function *Only) {
  if (Only && referencesVectorGlobal(*Only))
    throw std::runtime_error(
        "C projection references unsupported vector global storage");
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
        if (const auto *Allocation =
                llvm::dyn_cast<llvm::AllocaInst>(&Instruction))
          HasVectors |= containsVectorType(Allocation->getAllocatedType());
        HasVectors |= Instruction.getType()->isVectorTy();
        for (const auto &Operand : Instruction.operands())
          HasVectors |= Operand->getType()->isVectorTy();
      }
  }
  std::unique_ptr<llvm::Module> Projection;
  const llvm::Function *ProjectionOnly = Only;
  if (HasVectors) {
    if (llvm::verifyModule(Mod, &llvm::errs()))
      return false;
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
    lowerPackedVectorBitcasts(*Projection);
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
    llvm::ScalarizerPassOptions ScalarOptions;
    // LLVM only splits simple accesses: volatile and atomic vectors still
    // reach the unsupported-instruction guard below.
    ScalarOptions.ScalarizeLoadStore = true;
    Normalize.addPass(llvm::ScalarizerPass(ScalarOptions));
    Normalize.addPass(llvm::DCEPass());
    llvm::ModulePassManager Pipeline;
    Pipeline.addPass(
        llvm::createModuleToFunctionPassAdaptor(std::move(Normalize)));
    Pipeline.run(*Projection, Modules);
    // Lane-width-changing vector casts create new scalar/vector bitcasts in
    // Scalarizer. Lower those with the source layout before scalarizing their
    // explicit lane gathers, too. The second pass introduces no new casts.
    lowerPackedVectorBitcasts(*Projection);
    Modules.invalidate(*Projection, llvm::PreservedAnalyses::none());
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
      if (!isCVectorBoundarySignature(F.getFunctionType()))
        throw std::runtime_error(
            "C projection retains an unsupported vector signature");
      for (const auto &BB : F)
        for (const auto &I : BB) {
          if (const auto *Allocation = llvm::dyn_cast<llvm::AllocaInst>(&I))
            if (containsVectorType(Allocation->getAllocatedType()))
              throw std::runtime_error(
                  "C projection retains unsupported vector local storage");
          if ((containsVectorType(I.getType()) ||
               llvm::any_of(I.operands(),
                            [](const llvm::Use &Operand) {
                              return containsVectorType(Operand->getType());
                            })) &&
              !isCVectorBoundaryInstruction(I, Opts.TheArch))
            throw std::runtime_error(
                "C projection retains an unsupported vector instruction");
        }
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
