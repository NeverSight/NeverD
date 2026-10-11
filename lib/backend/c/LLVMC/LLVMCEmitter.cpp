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

#include "../FloatConversion.h"
#include "../FloatingPointContract.h"
#include "../UnalignedMemory.h"
#include "../VariadicImportStub.h"
#include "../pass/LLVMC/LLVMCCommonBranches.h"
#include "../pass/LLVMC/LLVMCLoopPhases.h"
#include "../pass/LLVMC/LLVMCScalarLoopRecovery.h"
#include "../render/X86FPStateHelpers.h"
#include "LLVMCFrameLayout.h"
#include "LLVMCIntegerMinMax.h"
#include "LLVMCScalarUnary.h"
#include "LLVMCWriter.h"

#include "neverd/Common.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/llvm/LLVMName.h"
#include "neverd/backend/llvm/LLVMSourceMap.h"
#include "neverd/backend/llvm/LLVMX86AddressSpaces.h"
#include "neverd/backend/llvm/LLVMX86FPStateAsm.h"
#include "neverd/backend/llvm/LLVMX86ShadowStackAsm.h"
#include "neverd/backend/llvm/PEImportShadow.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/IntrinsicsAArch64.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MD5.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Scalar/DCE.h"
#include "llvm/Transforms/Scalar/Scalarizer.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
#include "llvm/Transforms/Utils/PromoteMemToReg.h"

#define DEBUG_TYPE "neverd-llvmc-emitter"

#include <cctype>
#include <stdexcept>

namespace neverd {

namespace {
// A selected body also owns the constants it addresses, including globals
// synthesized by LLVM and providers named only by their initializers. Do not
// follow a referenced function into its body: that body is not being emitted.
std::set<const llvm::GlobalValue *>
referencedGlobals(const llvm::Function &Function) {
  std::set<const llvm::GlobalValue *> Globals;
  llvm::SmallVector<const llvm::Value *, 32> Work;
  llvm::SmallPtrSet<const llvm::Value *, 32> Seen;
  for (const auto &Block : Function)
    for (const auto &Instruction : Block)
      for (const auto &Operand : Instruction.operands())
        if (llvm::isa<llvm::Constant>(Operand))
          Work.push_back(Operand);
  while (!Work.empty()) {
    const auto *Value = Work.pop_back_val();
    if (!Seen.insert(Value).second)
      continue;
    if (const auto *Global = llvm::dyn_cast<llvm::GlobalValue>(Value)) {
      Globals.insert(Global);
      if (const auto *Object = llvm::dyn_cast<llvm::GlobalVariable>(Global);
          Object && Object->hasInitializer())
        Work.push_back(Object->getInitializer());
      continue;
    }
    if (const auto *Constant = llvm::dyn_cast<llvm::Constant>(Value))
      for (const auto &Operand : Constant->operands())
        Work.push_back(Operand);
  }
  return Globals;
}
} // namespace

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
  FunctionSymbolNames.clear();
  ExternalDataIdentifiers.clear();
  const std::set<std::string> ImportCNames =
      Img ? peImportCNames(*Img) : std::set<std::string>{};
  for (llvm::Function &Fn : Mod) {
    llvm::StringRef Name = Fn.getName();
    std::string IntrinsicHelper;
    if (Fn.getIntrinsicID() == llvm::Intrinsic::fshl ||
        Fn.getIntrinsicID() == llvm::Intrinsic::fshr) {
      IntrinsicHelper = Fn.getIntrinsicID() == llvm::Intrinsic::fshl
                            ? "neverd_llvm_fshl"
                            : "neverd_llvm_fshr";
      if (const auto *Integer =
              llvm::dyn_cast<llvm::IntegerType>(Fn.getReturnType()))
        IntrinsicHelper += "_i" + std::to_string(Integer->getBitWidth());
      Name = IntrinsicHelper;
    }
    if (const char *Kind = integerMinMaxSpelling(Fn.getIntrinsicID())) {
      IntrinsicHelper = std::string("neverd_llvm_") + Kind;
      if (const auto *Integer =
              llvm::dyn_cast<llvm::IntegerType>(Fn.getReturnType()))
        IntrinsicHelper += "_i" + std::to_string(Integer->getBitWidth());
      Name = IntrinsicHelper;
    }
    if (const char *Kind = scalarUnarySpelling(Fn.getIntrinsicID())) {
      IntrinsicHelper = std::string("neverd_llvm_") + Kind;
      if (const auto *Integer =
              llvm::dyn_cast<llvm::IntegerType>(Fn.getReturnType()))
        IntrinsicHelper += "_i" + std::to_string(Integer->getBitWidth());
      if (Fn.arg_size() == 1 && Fn.getArg(0)->getType()->isFloatingPointTy())
        IntrinsicHelper +=
            "_f" +
            std::to_string(Fn.getArg(0)->getType()->getPrimitiveSizeInBits());
      Name = IntrinsicHelper;
    }
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
          if (!DebugName.empty())
            if (auto Shadow =
                    peImportShadowName(ImportCNames, *Img, Addr, DebugName))
              DebugName = std::move(*Shadow);
          if (DebugName.empty())
            if (const Import *Imp = Img->findImportStubAt(Addr);
                Imp && !Imp->Name.empty())
              DebugName =
                  symbolOfImportName(Imp->Name, Opts.Format, Opts.TheArch);
        }
      }
    }
    // An image symbol carries the format's decoration; an LLVM name may have
    // lost it already.  A definition steps aside from the C runtime's names.
    llvm::StringRef CName =
        DebugName.empty() ? cNameOfGlobal(Name, Fn.isDeclaration())
                          : cNameOfSymbol(DebugName, Opts.Format, Opts.TheArch);
    FunctionSymbolNames.emplace(&Fn, CName.str());
    if (!Fn.isDeclaration())
      CName = cDefinitionName(CName, Opts.Format);
    FunctionIdentifiers.emplace(
        &Fn, GlobalIdentifierAllocator.allocate(CName, "nd_function"));
  }
  std::set<const llvm::GlobalVariable *> CallableGlobals;
  for (const auto &Function : Mod)
    for (const auto &Block : Function)
      for (const auto &Instruction : Block)
        if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction))
          if (const auto *Global = llvm::dyn_cast<llvm::GlobalVariable>(
                  Call->getCalledOperand()->stripPointerCasts()))
            CallableGlobals.insert(Global);
  for (const auto &Global : Mod.globals())
    if (Global.isDeclaration() && !Global.getName().empty() &&
        Global.getMetadata("neverd.data-symbol") &&
        !CallableGlobals.count(&Global) &&
        !parseNdDataSymbol(Global.getName()) &&
        !parseNdCodePtrSymbol(Global.getName()) &&
        !Global.getName().starts_with("__imp_") &&
        !Global.getName().starts_with("_imp_"))
      ExternalDataIdentifiers.emplace(
          &Global, GlobalIdentifierAllocator.allocate(
                       "neverd_data_" + Global.getName().str(), "neverd_data"));
}

std::string LLVMCWriter::functionIdentifier(const llvm::Function &Fn) const {
  if (auto It = FunctionIdentifiers.find(&Fn); It != FunctionIdentifiers.end())
    return It->second;
  llvm::StringRef CName = cNameOfGlobal(Fn.getName(), Fn.isDeclaration());
  if (!Fn.isDeclaration())
    CName = cDefinitionName(CName, Opts.Format);
  return canonicalizeCProjectionIdentifier(CName, "nd_function");
}

llvm::StringRef LLVMCWriter::cNameOfGlobal(llvm::StringRef Name,
                                           bool Declaration) const {
  if (Declaration && ImportEntryCNames.contains(Name))
    return Name;
  return llvm_name::cNameOfLLVMName(Name, Opts.Format, Opts.TheArch);
}

void LLVMCWriter::writeModule(llvm::Module &Mod, const llvm::Function *Only) {
  OnlyFunction = Only;
  SelectedGlobals =
      Only ? referencedGlobals(*Only) : std::set<const llvm::GlobalValue *>{};
  EmittedGlobalNames.clear();
  CurMod = &Mod;
  prepareFunctionIdentifiers(Mod);
  collectImageDataUses(Mod);
  writeIncludes(Mod);
  OS << "\n";
  writeStructDefs(Mod);
  for (const auto &Global : Mod.globals()) {
    if (Only && !SelectedGlobals.count(&Global))
      continue;
    const auto It = ExternalDataIdentifiers.find(&Global);
    if (It == ExternalDataIdentifiers.end())
      continue;
    const auto &Identifier = It->second;
    OS << "extern unsigned char " << Identifier << "[] __asm__(\""
       << escapeCString(cNameOfGlobal(Global.getName(), true)) << "\")";
    if (Global.hasExternalWeakLinkage())
      OS << " __attribute__((weak))";
    OS << ";\n";
  }
  auto WriteFunctions = [&] {
    for (auto &Fn : Mod) {
      if (Fn.isDeclaration())
        continue;
      if (OnlyFunction && &Fn != OnlyFunction)
        continue;
      auto Event = SourceRecorder ? SourceRecorder->function(Fn) : std::nullopt;
      if (SourceRecorder)
        OS << SourceRecorder->definition(Fn);
      if (Event)
        OS << SourceRecorder->begin(*Event);
      writeFunction(Fn);
      if (Event)
        OS << SourceRecorder->end(*Event);
      OS << "\n";
    }
  };
  if (OnlyFunction) {
    writeForwardDecls(Mod);
    writeGlobals(Mod);
    writeReferencedImageObjects(*OnlyFunction);
    writeImportCalleeDecls(Mod);
    WriteFunctions();
    return;
  }

  // A global initializer can reference any definition, including a later
  // one. Render each body once and retain its actual C prototype instead of
  // guessing that the LLVM type matches an inferred-void or debug projection.
  std::string Functions;
  llvm::raw_string_ostream FunctionsOS(Functions);
  {
    struct RestoreOutput {
      LLVMCOut &Out;
      llvm::raw_ostream *Original;
      ~RestoreOutput() { Out.retarget(Original); }
    } Restore{OS, &OS.stream()};
    OS.retarget(&FunctionsOS);
    WriteFunctions();
  }
  writeForwardDecls(Mod);
  writeGlobals(Mod);
  writeImportCalleeDecls(Mod);
  OS << "\n" << Functions;
}

void LLVMCWriter::writeIncludes(llvm::Module &Mod) {
  bool HasFloatingArithmetic = false;
  std::set<std::string> Headers;
  Headers.insert("stdint.h");
  NeedsUnalignedTypes = false;
  std::map<std::string, std::pair<unsigned, bool>> FunnelShifts;
  std::map<std::string, ScalarIntegerMinMax> IntegerMinMax;
  std::map<std::string, ScalarUnary> ScalarUnaries;
  FPStateHelperNames.clear();

  for (auto &Fn : Mod) {
    if (OnlyFunction && &Fn != OnlyFunction)
      continue;
    for (auto &BB : Fn) {
      for (auto &Inst : BB) {
        HasFloatingArithmetic |= Inst.getOpcode() == llvm::Instruction::FAdd ||
                                 Inst.getOpcode() == llvm::Instruction::FSub ||
                                 Inst.getOpcode() == llvm::Instruction::FMul ||
                                 Inst.getOpcode() == llvm::Instruction::FDiv;
        if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Inst)) {
          if (auto Shape = scalarIntegerMinMax(*Call))
            IntegerMinMax.emplace(
                functionIdentifier(*Call->getCalledFunction()), *Shape);
          if (auto Shape = scalarUnary(*Call))
            ScalarUnaries.emplace(
                functionIdentifier(*Call->getCalledFunction()), *Shape);
        }
        if (llvm::isa<llvm::FenceInst>(&Inst))
          HasCIntrinsics = true;
        if (auto *LI = llvm::dyn_cast<llvm::LoadInst>(&Inst)) {
          if (isLLVMX86SegmentedAddressSpace(LI->getPointerAddressSpace()))
            HasCIntrinsics = true;
          NeedsUnalignedTypes |=
              LI->isSimple() && LI->getPointerAddressSpace() == 0 &&
              !c_memory::alias(typeToCLLVM(LI->getType()), Opts.ScalarPointers)
                   .empty();
        }
        if (const auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst))
          NeedsUnalignedTypes |=
              SI->isSimple() && SI->getPointerAddressSpace() == 0 &&
              !c_memory::alias(typeToCLLVM(SI->getValueOperand()->getType()),
                               Opts.ScalarPointers)
                   .empty();
        if (auto *CI = llvm::dyn_cast<llvm::CallInst>(&Inst)) {
          if (classifyX86ShadowStackReadAsm(*CI)) {
            const unsigned Word = CI->getType()->getIntegerBitWidth() / 8;
            if ((Opts.TheArch != Arch::X86 && Opts.TheArch != Arch::X64) ||
                Word != (Opts.TheArch == Arch::X64 ? 8U : 4U))
              llvm::report_fatal_error(
                  "shadow stack read target/type mismatch");
          }
          if (auto Shape = classifyX86FPStateAsm(*CI);
              Shape && isX86FPNumericalStateIntrinsic(Shape->first)) {
            if (Opts.TheArch == Arch::X86 &&
                isX86FPConversionStateIntrinsic(Shape->first) &&
                x86FPStateDestinationBytes(Shape->first, Shape->second) == 8)
              llvm::report_fatal_error(
                  "x86-32 FP conversion requires a 32-bit integer result");
            auto [It, Inserted] = FPStateHelperNames.try_emplace(*Shape);
            if (Inserted)
              It->second = GlobalIdentifierAllocator.allocate(
                  x86FPScalarValueCHelper(Shape->first, Shape->second),
                  "nd_fp_value");
          }
          if (const auto *Asm =
                  llvm::dyn_cast<llvm::InlineAsm>(CI->getCalledOperand())) {
            if (Asm->getAsmString().starts_with("pushf") ||
                Asm->getAsmString() == "pushq $0\n\tpopfq" ||
                Asm->getAsmString() == "pushl $0\n\tpopfl")
              Headers.insert(Opts.Format == BinaryFormat::COFF ? "intrin.h"
                                                               : "x86intrin.h");
            if ((Opts.TheArch == Arch::X86 || Opts.TheArch == Arch::X64) &&
                (Asm->getAsmString() == "ldmxcsr ($0)" ||
                 Asm->getAsmString() == "stmxcsr ($0)"))
              HasCIntrinsics = true;
            continue;
          }
          auto *Callee = CI->getCalledFunction();
          if (!Callee)
            continue;
          const auto IID = Callee->getIntrinsicID();
          if (IID == llvm::Intrinsic::memcpy ||
              IID == llvm::Intrinsic::memmove || IID == llvm::Intrinsic::memset)
            Headers.insert("string.h");
          if (IID == llvm::Intrinsic::fshl || IID == llvm::Intrinsic::fshr) {
            const auto *Ty = llvm::dyn_cast<llvm::IntegerType>(CI->getType());
            const unsigned Width = Ty ? Ty->getBitWidth() : 0;
            if (CI->arg_size() != 3 ||
                (Width != 8 && Width != 16 && Width != 32 && Width != 64 &&
                 Width != 128))
              throw std::runtime_error("unsupported LLVM funnel shift width");
            FunnelShifts.emplace(
                functionIdentifier(*Callee),
                std::pair{Width, IID == llvm::Intrinsic::fshl});
          }
          auto Name = Callee->getName().str();

          if (const char *Mapped = llvmIntrinsicToCName(Name.c_str())) {
            // Generic arithmetic and control builtins use host C facilities.
            // Only mappings that actually name ISA intrinsics need the
            // recovered image's architecture headers.
            const llvm::StringRef RawName(Name);
            if (RawName.starts_with("llvm.x86.") ||
                RawName.starts_with("llvm.aarch64.") ||
                RawName.starts_with("llvm.arm.") ||
                IID == llvm::Intrinsic::readcyclecounter ||
                IID == llvm::Intrinsic::debugtrap)
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

  // A stub of a variadic import passes its arguments on through stdarg.h.
  if (Img)
    for (llvm::Function &Fn : Mod) {
      if (Fn.isDeclaration())
        continue;
      auto VA = rewrite_source::getOriginalVA(Fn);
      if (!VA) {
        llvm::consumeError(VA.takeError());
        continue;
      }
      if (!*VA)
        continue;
      if (const std::string Import = c_stub::variadicImportOfStub(*Img, **VA);
          !Import.empty())
        if (const auto *Forward = libc::libcVariadicForward(Import))
          c_stub::addVariadicStubHeaders(Headers, *Forward);
    }

  if (HasCIntrinsics)
    for (const char *Hdr : getArchIntrinsicHeaders(Opts.TheArch))
      Headers.insert(Hdr);

  if (Opts.EmitIncludes) {
    for (auto &H : Headers)
      OS << "#include <" << H << ">\n";
    OS << "\n";
  }
  if (HasFloatingArithmetic)
    c_float::writeContractionPolicy(OS.stream());
  // The accesses' types or assumptions are written only when an access can
  // name a type; otherwise every access keeps its portable byte copy.
  UnalignedTypesWritten = Opts.UseUnalignedPointers && NeedsUnalignedTypes;
  if (UnalignedTypesWritten)
    c_memory::writeTypes(OS, Opts.ScalarPointers);
  for (const auto &[Name, Shape] : ScalarUnaries) {
    const unsigned CarrierBits = Shape.carrierBits();
    const std::string Type = CarrierBits == 128
                                 ? "unsigned __int128"
                                 : "uint" + std::to_string(CarrierBits) + "_t";
    if (Shape.Kind == llvm::Intrinsic::bitreverse) {
      OS << "static inline " << Type << " " << Name << "(" << Type
         << " value) {\n"
         << "    " << Type << " result = 0;\n"
         << "    for (unsigned int bit = 0; bit < " << Shape.Bits
         << "; ++bit) {\n"
         << "        result = (" << Type << ")((result << 1) | (value & 1));\n"
         << "        value >>= 1;\n"
         << "    }\n    return result;\n}\n\n";
      continue;
    }
    c_float::writeConversion(OS, Name,
                             {Shape.Bits, Shape.FloatBits,
                              Shape.Kind == llvm::Intrinsic::fptosi_sat,
                              FPToIntegerPolicy::Saturate});
  }
  writeX86FPScalarValueCHelpers(OS, FPStateHelperNames);
  for (const auto &[Name, Shape] : IntegerMinMax) {
    const unsigned CarrierBits = Shape.Bits == 1 ? 8 : Shape.Bits;
    const std::string Type = CarrierBits == 128
                                 ? "unsigned __int128"
                                 : "uint" + std::to_string(CarrierBits) + "_t";
    OS << "static inline " << Type << " " << Name << "(" << Type << " a, "
       << Type << " b) {\n";
    if (Shape.Bits == 1)
      OS << "    a &= 1;\n    b &= 1;\n";
    if (Shape.Signed) {
      // Flipping the sign bit maps two's-complement signed order to unsigned
      // order without an out-of-range signed cast or a 128-bit truncation.
      OS << "    const " << Type << " sign = (" << Type << ")1 << "
         << Shape.Bits - 1 << ";\n"
         << "    return (a ^ sign) " << (Shape.Minimum ? '<' : '>')
         << " (b ^ sign) ? a : b;\n";
    } else {
      OS << "    return a " << (Shape.Minimum ? '<' : '>') << " b ? a : b;\n";
    }
    OS << "}\n\n";
  }
  for (const auto &[Name, Shape] : FunnelShifts) {
    const auto [Width, Left] = Shape;
    const std::string Type = Width == 128
                                 ? "unsigned __int128"
                                 : "uint" + std::to_string(Width) + "_t";
    OS << "static inline " << Type << " " << Name << "(" << Type << " a, "
       << Type << " b, " << Type << " amount) {\n"
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
  std::set<llvm::Type *> Seen;
  std::function<void(llvm::Type *)> Visit = [&](llvm::Type *Ty) {
    if (!Seen.insert(Ty).second)
      return;
    if (auto *FT = llvm::dyn_cast<llvm::FunctionType>(Ty)) {
      Visit(FT->getReturnType());
      for (auto *Parameter : FT->params())
        Visit(Parameter);
      return;
    }
    if (!Ty->isAggregateType())
      return;
    if (auto *ST = llvm::dyn_cast<llvm::StructType>(Ty); ST && ST->isOpaque()) {
      OS << typeToCLLVM(Ty) << ";\n";
      return;
    }
    for (auto *Child : Ty->subtypes())
      Visit(Child);
    // Selected-function fragments may be combined in one C translation unit.
    // Include the complete LLVM shape, so equal records share a definition
    // while incompatible named records still produce a C redefinition error.
    std::string Shape;
    llvm::raw_string_ostream ShapeOut(Shape);
    Ty->print(ShapeOut);
    llvm::MD5 Hash;
    Hash.update(Shape);
    const std::string Guard =
        "NEVERD_C_AGGREGATE_" + Hash.final().digest().str().str();
    OS << "#ifndef " << Guard << "\n#define " << Guard << "\n";
    OS << typeToCLLVM(Ty) << " {\n";
    if (auto *Array = llvm::dyn_cast<llvm::ArrayType>(Ty)) {
      OS << "    " << typeToCLLVM(Array->getElementType()) << " elements["
         << Array->getNumElements() << "];\n";
    } else {
      auto *ST = llvm::cast<llvm::StructType>(Ty);
      for (unsigned I = 0; I < ST->getNumElements(); ++I)
        OS << "    " << typeToCLLVM(ST->getElementType(I)) << " field_" << I
           << ";\n";
    }
    OS << "}";
    if (auto *ST = llvm::dyn_cast<llvm::StructType>(Ty); ST && ST->isPacked())
      OS << " __attribute__((packed))";
    OS << ";\n#endif\n";
    const auto &Layout = Mod.getDataLayout();
    OS << "_Static_assert(sizeof(" << typeToCLLVM(Ty)
       << ") == " << Layout.getTypeAllocSize(Ty)
       << ", \"LLVM aggregate size\");\n";
    if (auto *ST = llvm::dyn_cast<llvm::StructType>(Ty)) {
      const auto *RecordLayout = Layout.getStructLayout(ST);
      for (unsigned I = 0; I < ST->getNumElements(); ++I)
        OS << "_Static_assert(__builtin_offsetof(" << typeToCLLVM(Ty)
           << ", field_" << I << ") == " << RecordLayout->getElementOffset(I)
           << ", \"LLVM aggregate field offset\");\n";
    }
    OS << "\n";
  };
  auto VisitStorage = [&](llvm::Type *Ty) {
    if (auto *Array = llvm::dyn_cast<llvm::ArrayType>(Ty))
      Visit(Array->getElementType());
    else
      Visit(Ty);
  };
  for (auto &Global : Mod.globals())
    VisitStorage(Global.getValueType());
  for (auto &Fn : Mod) {
    Visit(Fn.getFunctionType());
    if (OnlyFunction && &Fn != OnlyFunction)
      continue;
    for (auto &BB : Fn)
      for (auto &Inst : BB) {
        Visit(Inst.getType());
        if (auto *Allocation = llvm::dyn_cast<llvm::AllocaInst>(&Inst))
          VisitStorage(Allocation->getAllocatedType());
        for (const auto &Operand : Inst.operands())
          Visit(Operand->getType());
      }
  }
}

void LLVMCWriter::writeImageByteArray(const llvm::GlobalVariable &Global,
                                      llvm::StringRef Name) {
  const auto *Array = llvm::cast<llvm::ArrayType>(Global.getValueType());
  if (Global.hasLocalLinkage())
    OS << "static ";
  if (Global.isConstant())
    OS << "const ";
  OS << "uint8_t " << Name << "[" << Array->getNumElements() << "] = {";
  bool Any = false;
  if (!Global.getInitializer()->isNullValue())
    for (uint64_t I = 0; I < Array->getNumElements(); ++I) {
      const auto *Element = llvm::dyn_cast_or_null<llvm::ConstantInt>(
          Global.getInitializer()->getAggregateElement(
              static_cast<unsigned>(I)));
      if (!Element)
        throw std::invalid_argument(
            "non-byte initializer in image byte storage");
      if (!Element->isZero()) {
        OS << (Any ? ", " : "") << "[" << I
           << "] = " << Element->getZExtValue();
        Any = true;
      }
    }
  if (!Any)
    OS << "0";
  OS << "};\n";
}

void LLVMCWriter::writeGlobals(llvm::Module &Mod) {
  if (OnlyFunction)
    for (const auto &Global : Mod.globals()) {
      if (!SelectedGlobals.count(&Global) || !Global.hasInitializer())
        continue;
      // Initializers can refer forward or form cycles. The declarations use
      // the same storage shape and linkage as the definitions below.
      OS << (Global.hasLocalLinkage() ? "static " : "extern ");
      if (Global.isConstant())
        OS << "const ";
      auto *Array = llvm::dyn_cast<llvm::ArrayType>(Global.getValueType());
      const auto *Data =
          llvm::dyn_cast<llvm::ConstantDataArray>(Global.getInitializer());
      const bool String = Global.isConstant() && Data && Data->isString() &&
                          !parseNdDataSymbol(Global.getName());
      OS << (String ? "char"
                    : typeToCLLVM(Array ? Array->getElementType()
                                        : Global.getValueType()))
         << " " << constStr(&Global);
      if (Array)
        OS << "[" << Array->getNumElements() << "]";
      OS << ";\n";
    }
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
    if (OnlyFunction && !SelectedGlobals.count(&GV))
      continue;
    std::string RawName = GV.getName().str();
    if (RawName.empty())
      continue;

    if (ExternalDataIdentifiers.count(&GV))
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
      EmittedGlobalNames.insert(Name);
      continue;
    }

    if (!GV.hasInitializer())
      continue;

    auto *Init = GV.getInitializer();
    EmittedGlobalNames.insert(Name);

    // Lifted writable image data can be one byte array with overlapping
    // integer views.  Keep the backing range as an array; printing it as a
    // pointer, or printing each offset as a separate C object, loses those
    // overlaps (including the two ten-byte x87 operands in FPREM samples).
    if (const auto *Array = llvm::dyn_cast<llvm::ArrayType>(GV.getValueType());
        parseNdDataSymbol(RawName) && Array &&
        Array->getElementType()->isIntegerTy(8) &&
        (llvm::isa<llvm::ConstantAggregateZero>(Init) ||
         llvm::isa<llvm::ConstantDataArray>(Init))) {
      writeImageByteArray(GV, Name);
      continue;
    }

    if (auto *CDA = llvm::dyn_cast<llvm::ConstantDataArray>(Init)) {
      if (GV.isConstant() && CDA->isString()) {
        llvm::StringRef Raw = CDA->getAsString();
        while (!Raw.empty() && Raw.back() == '\0')
          Raw = Raw.drop_back();
        if (GV.hasLocalLinkage())
          OS << "static ";
        OS << "const char " << Name << "[] = \"" << escapeCString(Raw)
           << "\";\n";
        continue;
      }
    }

    // Storage arrays retain C array declarations; only SSA values need the
    // record carrier. GEPs and image byte ranges still address this storage.
    if (auto *Array = llvm::dyn_cast<llvm::ArrayType>(GV.getValueType())) {
      if (GV.hasLocalLinkage())
        OS << "static ";
      if (GV.isConstant())
        OS << "const ";
      OS << typeToCLLVM(Array->getElementType()) << " " << Name << "["
         << Array->getNumElements() << "] = {";
      if (llvm::isa<llvm::ConstantAggregateZero>(Init))
        OS << "0";
      else
        for (uint64_t I = 0; I < Array->getNumElements(); ++I) {
          if (I)
            OS << ", ";
          OS << constStr(Init->getAggregateElement(unsigned(I)));
        }
      OS << "};\n";
      continue;
    }

    if (GV.hasLocalLinkage())
      OS << "static ";
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
      if (EmittedGlobalNames.count(constStr(Backing->first)))
        return;
      ByteArrays.emplace(*parseNdDataSymbol(Backing->first->getName()),
                         Backing->first);
      return;
    }
    std::string Name = imageDataCName(Ptr);
    if (Name.empty() || EmittedGlobalNames.count(Name))
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
  for (const auto &[Base, GV] : ByteArrays)
    writeImageByteArray(*GV, namedImageObject(Base));
  if (!Objs.empty() || !ByteArrays.empty())
    OS << "\n";
}

void LLVMCWriter::writeImportCalleeDecls(llvm::Module &Mod) {
  std::set<std::string> Declared;
  for (const llvm::Function &Fn : Mod)
    Declared.insert(functionIdentifier(Fn));
  // Each import with the type its calls return; one whose calls disagree is
  // left to them.
  std::map<std::string, std::optional<std::string>> Imports;
  for (const llvm::Function &Fn : Mod) {
    if (Fn.isDeclaration() || (OnlyFunction && &Fn != OnlyFunction))
      continue;
    for (const llvm::BasicBlock &Block : Fn)
      for (const llvm::Instruction &Inst : Block) {
        const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Inst);
        if (!Call || Call->getCalledFunction() ||
            llvm::isa<llvm::InlineAsm>(Call->getCalledOperand()))
          continue;
        // Only a slot the loader binds outside the import table names an
        // import no header declares (an ELF GOT entry); a table's imports
        // keep their headers' prototypes.
        const llvm::Value *Slot = Call->getCalledOperand()->stripPointerCasts();
        if (const auto *Cast = llvm::dyn_cast<llvm::IntToPtrInst>(Slot))
          Slot = Cast->getOperand(0);
        const auto *Load = llvm::dyn_cast<llvm::LoadInst>(Slot);
        const auto SlotVA =
            Load ? imageDataVA(Load->getPointerOperand()) : std::nullopt;
        if (!Img || !SlotVA || !Img->ImportStorageSlots.count(*SlotVA) ||
            Img->findImportAt(*SlotVA))
          continue;
        std::string Name = resolveImportCalleeName(Call->getCalledOperand());
        if (Name.empty() || Declared.count(Name))
          continue;
        const std::string Return = typeToCLLVM(Call->getType());
        const auto [Import, Added] = Imports.emplace(Name, Return);
        if (!Added && Import->second && *Import->second != Return)
          Import->second.reset();
      }
  }
  // Unprototyped, the declaration takes the arguments the machine passed.
  for (const auto &[Name, Return] : Imports)
    if (Return)
      OS << "extern " << *Return << " " << Name << "();\n";
}

void LLVMCWriter::collectImageDataUses(llvm::Module &Mod) {
  // Image data is named by its address, from the image or from the module's
  // data globals.
  ImageDataUses.clear();
  const llvm::DataLayout &Layout = Mod.getDataLayout();
  for (const llvm::Function &Fn : Mod)
    for (const llvm::BasicBlock &Block : Fn)
      for (const llvm::Instruction &Inst : Block) {
        const llvm::Value *Pointer = nullptr;
        llvm::Type *Type = nullptr;
        if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Inst)) {
          Pointer = Load->getPointerOperand();
          Type = Load->getType();
        } else if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Inst)) {
          Pointer = Store->getPointerOperand();
          Type = Store->getValueOperand()->getType();
        } else if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Inst)) {
          // A call through a pointer the image holds.
          if (Call->getCalledFunction())
            continue;
          const llvm::Value *Callee =
              Call->getCalledOperand()->stripPointerCasts();
          if (const auto *Cast = llvm::dyn_cast<llvm::IntToPtrInst>(Callee))
            Callee = Cast->getOperand(0);
          if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(Callee))
            if (auto VA = imageDataVA(Load->getPointerOperand()))
              ImageDataUses[*VA].CallSlot = true;
          continue;
        }
        if (!Pointer || !Type || !Type->isSized())
          continue;
        if (auto VA = imageDataVA(Pointer))
          ImageDataUses[*VA].AccessBytes.insert(
              Layout.getTypeStoreSize(Type).getFixedValue());
      }
}

void LLVMCWriter::writeForwardDecls(llvm::Module &Mod) {
  for (auto &Fn : Mod) {
    if (OnlyFunction) {
      // Selected fragments need declarations for their referenced providers,
      // including scalar calls and definitions whose bodies are not emitted.
      if (!SelectedGlobals.count(&Fn))
        continue;
    }
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

    if (!OnlyFunction && !Fn.isDeclaration()) {
      if (auto It = DefinitionDeclarations.find(&Fn);
          It != DefinitionDeclarations.end()) {
        OS << It->second << ";\n";
        continue;
      }
      if (!Opts.PreserveLLVMFunctionTypes)
        continue;
    }

    if (libc::isKnownFunction(Name))
      continue;
    if (const MsvcCallee *Msvc = msvcCallee(Name, Opts.Format)) {
      OS << msvcSyntheticPrototype(Name, *Msvc,
                                   Opts.TheArch == Arch::X64 && Msvc->FastCall)
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
    OS << ")";
    // An import whose identifier is not its symbol links by the symbol, which
    // a comment spells as its language does.
    std::string Comment;
    if (Fn.isDeclaration()) {
      const llvm::StringRef CName = cNameOfGlobal(RawName, true);
      if (linksByLabel(CName, Name)) {
        std::string Label;
        llvm::raw_string_ostream(Label).write_escaped(
            symbolOfCName(CName, Opts.Format, Opts.TheArch));
        OS << " __asm__(\"" << Label << "\")";
        // A name that reads as the label spells it needs no comment.
        if (const std::string Readable = demangledComment(CName);
            Opts.EmitComments && !Readable.empty() && Readable != CName)
          Comment = " /* " + Readable + " */";
      }
    }
    OS << ";" << Comment << "\n";
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
      return Type->isIntegerTy() || Type->isHalfTy() || Type->isFloatTy() ||
             Type->isDoubleTy() || Type->isBFloatTy();
    };
    if (!Vector || !IsLane(Scalar) || !IsLane(Vector->getElementType()))
      continue;
    const unsigned ScalarBits = Scalar->getPrimitiveSizeInBits();
    const unsigned Count = Vector->getNumElements();
    const unsigned Bits = Vector->getElementType()->getPrimitiveSizeInBits();
    if (ScalarBits > 512 || uint64_t(Count) * Bits != ScalarBits)
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
  if (const auto *Call = llvm::dyn_cast<llvm::CallInst>(&Inst))
    if (const auto Shape = classifyX86FPStateAsm(*Call);
        Shape && isX86FPNumericalStateIntrinsic(Shape->first))
      return TheArch == Arch::X86 || TheArch == Arch::X64;
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
    return (!Callee || !Callee->isIntrinsic()) &&
           isCVectorBoundarySignature(Call->getFunctionType());
  }
  return false;
}

static bool containsArrayValue(llvm::Type *Ty) {
  if (Ty->isArrayTy())
    return true;
  if (auto *ST = llvm::dyn_cast<llvm::StructType>(Ty); ST && !ST->isOpaque())
    return llvm::any_of(ST->elements(), containsArrayValue);
  return false;
}

static void validateArrayValueBoundaries(const llvm::Module &Mod,
                                         const llvm::Function *Only) {
  const auto &Target = Mod.getTargetTriple();
  // A C record is a value carrier, but that alone does not authenticate its
  // ABI against a raw LLVM aggregate. Admit the word pairs for which both
  // lower to one/two integer argument/result registers. Larger, narrow-lane,
  // nested and Windows aggregates need a separate ABI lowering contract.
  const bool WordABI =
      (Target.isOSLinux() || Target.isOSDarwin()) &&
      (Target.isAArch64() || Target.getArch() == llvm::Triple::x86_64) &&
      Mod.getDataLayout().getPointerSizeInBits() == 64;
  auto CheckType = [&](llvm::Type *Ty) {
    if (!containsArrayValue(Ty))
      return;
    const auto *Array = llvm::dyn_cast<llvm::ArrayType>(Ty);
    if (!WordABI || !Array || !Array->getElementType()->isIntegerTy(64) ||
        Array->getNumElements() < 1 || Array->getNumElements() > 2)
      throw std::runtime_error(
          "C projection has an unsupported array value ABI");
  };
  auto CheckSignature = [&](llvm::FunctionType *Ty, unsigned CC,
                            const llvm::AttributeList &Attributes) {
    bool HasArray = containsArrayValue(Ty->getReturnType());
    bool HasArrayParameter = false;
    unsigned ArgumentWords = 0;
    bool WordParameters = true;
    CheckType(Ty->getReturnType());
    for (auto *Param : Ty->params()) {
      HasArray |= containsArrayValue(Param);
      HasArrayParameter |= containsArrayValue(Param);
      CheckType(Param);
      if (auto *Array = llvm::dyn_cast<llvm::ArrayType>(Param))
        ArgumentWords += Array->getNumElements();
      else if (Param->isIntegerTy(64) ||
               (Param->isPointerTy() && Param->getPointerAddressSpace() == 0))
        ++ArgumentWords;
      else
        WordParameters = false;
    }
    if (HasArray && CC != llvm::CallingConv::C)
      throw std::runtime_error(
          "C projection has an unsupported array calling convention");
    if (HasArray)
      for (auto Set : Attributes)
        for (auto Kind :
             {llvm::Attribute::ByVal, llvm::Attribute::ByRef,
              llvm::Attribute::InAlloca, llvm::Attribute::Preallocated,
              llvm::Attribute::StructRet, llvm::Attribute::InReg,
              llvm::Attribute::Nest, llvm::Attribute::SwiftSelf,
              llvm::Attribute::SwiftError})
          if (Set.hasAttribute(Kind))
            throw std::runtime_error(
                "C projection has unsupported array ABI attributes");
    // Once registers run out, C may spill a whole record while LLVM splits
    // the raw aggregate. Do not infer that stack-argument ABI from its type.
    const unsigned RegisterWords = Target.isAArch64() ? 8 : 6;
    if (HasArrayParameter &&
        (!WordParameters || ArgumentWords > RegisterWords || Ty->isVarArg()))
      throw std::runtime_error(
          "C projection has an unsupported array argument ABI");
  };
  for (const auto &Fn : Mod) {
    if (Only && &Fn != Only)
      continue;
    CheckSignature(Fn.getFunctionType(), Fn.getCallingConv(),
                   Fn.getAttributes());
    for (const auto &BB : Fn)
      for (const auto &Inst : BB)
        if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Inst)) {
          CheckSignature(Call->getFunctionType(), Call->getCallingConv(),
                         Call->getAttributes());
          for (unsigned I = Call->getFunctionType()->getNumParams();
               I < Call->arg_size(); ++I)
            if (containsArrayValue(Call->getArgOperand(I)->getType()))
              throw std::runtime_error(
                  "C projection has an unsupported variadic array value");
        }
  }
}

// Keep call-result register homes in SSA on the private scalar projection.
// Repeating after promotion also exposes narrow views of the same result;
// LLVM owns the dominance/initialization proof for every promoted load.
static void promoteCallResultHomes(llvm::Function &Function) {
  llvm::DominatorTree Dominators(Function);
  for (unsigned Round = 0; Round != 8; ++Round) {
    llvm::SmallVector<llvm::AllocaInst *, 8> Homes;
    for (auto &Instruction : Function.getEntryBlock()) {
      auto *Home = llvm::dyn_cast<llvm::AllocaInst>(&Instruction);
      if (!Home || !llvm::isAllocaPromotable(Home))
        continue;
      const llvm::StoreInst *Definition = nullptr;
      bool Multiple = false;
      bool Read = false;
      for (auto *User : Home->users())
        if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(User)) {
          Multiple |= Definition != nullptr;
          Definition = Store;
        } else {
          Read |= llvm::isa<llvm::LoadInst>(User);
        }
      if (!Read || Multiple || !Definition)
        continue;
      const llvm::Value *Value = Definition->getValueOperand();
      unsigned Depth = 0;
      while (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(Value)) {
        if (++Depth > 8)
          break;
        Value = Cast->getOperand(0);
      }
      if (llvm::isa<llvm::CallInst>(Value))
        Homes.push_back(Home);
    }
    if (Homes.empty())
      break;
    llvm::PromoteMemToReg(Homes, Dominators);
  }
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
        // Validate explicitly supported scalar shapes before normalization can
        // discard an unused malformed call, retaining the source error
        // contract.
        if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction)) {
          (void)scalarIntegerMinMax(*Call);
          (void)scalarUnary(*Call);
        }
        if (const auto *Allocation =
                llvm::dyn_cast<llvm::AllocaInst>(&Instruction))
          HasVectors |= containsVectorType(Allocation->getAllocatedType());
        HasVectors |= Instruction.getType()->isVectorTy();
        for (const auto &Operand : Instruction.operands())
          HasVectors |= Operand->getType()->isVectorTy();
      }
  }
  if (llvm::verifyModule(Mod, &llvm::errs()))
    return false;
  validateArrayValueBoundaries(Mod, Only);
  // Source normalization must not mutate the caller's IR. Remove dead
  // computations left by recovered control flow even when semantic
  // optimization is disabled; they can otherwise reference unneeded image
  // tables. LLVM DCE retains volatile/atomic accesses and side-effecting calls.
  llvm::ValueToValueMapTy ValueMap;
  auto Projection = llvm::CloneModule(Mod, ValueMap);
  // Normalization can remove an otherwise unused frame_end/rsp_init marker.
  // Capture its display context on the private allocation before DCE so
  // debug stack offsets still name the same bytes after normalization.
  for (auto &Function : *Projection)
    for (auto &Block : Function)
      for (auto &Instruction : Block)
        if (auto *Frame = llvm::dyn_cast<llvm::AllocaInst>(&Instruction))
          if (auto Base = llvmc::syntheticFrameBaseOffset(
                  *Frame, Projection->getDataLayout()))
            Frame->setMetadata(
                llvmc::CapturedFrameBase,
                llvm::MDNode::get(
                    Mod.getContext(),
                    llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
                        llvm::Type::getInt64Ty(Mod.getContext()), *Base))));
  std::optional<LLVMSourceMap> Sources;
  if (Opts.SourceMap && Opts.SourceMap->LLVMSources) {
    Sources = *Opts.SourceMap->LLVMSources;
    Sources->remap([&](llvm::Value *Value) { return ValueMap.lookup(Value); });
  }
  const llvm::Function *ProjectionOnly = nullptr;
  if (Only) {
    ProjectionOnly =
        llvm::dyn_cast_or_null<llvm::Function>(ValueMap.lookup(Only));
    if (!ProjectionOnly)
      throw std::runtime_error(
          "C projection could not map the selected function into its clone");
  }
  if (HasVectors) {
    lowerCIntegerReductions(*Projection);
    lowerPackedVectorBitcasts(*Projection);
  }
  // Native image/debug projections keep their separate source contracts.
  // Pure scalar loop recovery proves its exact replacement before publishing
  // into this private clone and retains the existing path on refusal.
  if (!Dbg && !Img && !Opts.Image)
    llvmc::recoverScalarLoops(*Projection, ProjectionOnly);
  if (!Dbg && !Img && !Opts.Image)
    for (auto &Function : *Projection)
      if (!Function.isDeclaration() &&
          (!ProjectionOnly || &Function == ProjectionOnly))
        promoteCallResultHomes(Function);
  for (auto &Function : *Projection)
    if (!ProjectionOnly || &Function == ProjectionOnly) {
      llvmc::factorCommonBranchTests(Function);
      llvmc::factorLoopPhiExpressions(Function);
    }
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
  if (HasVectors) {
    llvm::ScalarizerPassOptions ScalarOptions;
    // LLVM only splits simple accesses: volatile and atomic vectors still
    // reach the unsupported-instruction guard below.
    ScalarOptions.ScalarizeLoadStore = true;
    Normalize.addPass(llvm::ScalarizerPass(ScalarOptions));
  }
  Normalize.addPass(llvm::DCEPass());
  llvm::ModulePassManager Pipeline;
  Pipeline.addPass(
      llvm::createModuleToFunctionPassAdaptor(std::move(Normalize)));
  Pipeline.run(*Projection, Modules);
  if (HasVectors) {
    // Lane-width-changing vector casts create new scalar/vector bitcasts in
    // Scalarizer. Lower those with the source layout before scalarizing their
    // explicit lane gathers, too. The second pass introduces no new casts.
    lowerPackedVectorBitcasts(*Projection);
    Modules.invalidate(*Projection, llvm::PreservedAnalyses::none());
    Pipeline.run(*Projection, Modules);
  }
  if (llvm::verifyModule(*Projection, &llvm::errs()))
    return false;
  if (HasVectors) {
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
              !isCVectorBoundaryInstruction(I, Opts.TheArch)) {
            std::string Detail;
            llvm::raw_string_ostream Stream(Detail);
            Stream
                << "C projection retains an unsupported vector instruction in "
                << F.getName() << ": " << I;
            throw std::runtime_error(Stream.str());
          }
        }
    }
  }
  if (!Opts.SourceMap) {
    LLVMCWriter W(Out, Opts, Dbg, Img);
    W.writeModule(*Projection, ProjectionOnly);
    return true;
  }
  std::string Ordinary, Annotated;
  llvm::raw_string_ostream OrdinaryOS(Ordinary), AnnotatedOS(Annotated);
  LLVMCWriter Plain(OrdinaryOS, Opts, Dbg, Img);
  Plain.writeModule(*Projection, ProjectionOnly);
  CSourceRecorder Recorder(*Opts.SourceMap, Ordinary);
  try {
    if (Sources)
      Recorder.prepareLLVMSources(*Sources);
    LLVMCWriter Marked(AnnotatedOS, Opts, Dbg, Img, true, &Recorder);
    Marked.writeModule(*Projection, ProjectionOnly);
    Recorder.finish(Annotated, Ordinary);
  } catch (const std::exception &) {
    // A mapping failure leaves the successful ordinary source intact, with
    // unknown spans. Errors in the ordinary emission still propagate.
  }
  CSourceRecorder Instructions(*Opts.SourceMap, Ordinary, true);
  try {
    if (Sources)
      Instructions.prepareLLVMSources(*Sources);
    std::string Annotated;
    llvm::raw_string_ostream OS(Annotated);
    LLVMCWriter Marked(OS, Opts, Dbg, Img, true, &Instructions);
    Marked.writeModule(*Projection, ProjectionOnly);
    Instructions.finish(Annotated, Ordinary);
  } catch (const std::exception &) {
    // Navigation evidence does not authorize changes to source/library mapping.
  }
  Out << Ordinary;
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
