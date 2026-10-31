//===- X86Virtualization.cpp - VMX/SVM source rendering ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Render the owned architectural assembly contract without speculating about
/// VMCS status or materializing conditional memory operands as C accesses.
//===----------------------------------------------------------------------===//

#include "X86Virtualization.h"

#include "neverd/ir/high/HighIR.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/ModRef.h"

#include <stdexcept>

namespace neverd {
namespace {

std::string unsignedType(uint16_t Bytes) {
  return Bytes == 16 ? "__uint128_t"
                     : "uint" + std::to_string(8 * Bytes) + "_t";
}

std::string gnuConstraint(const std::string &Constraint) {
  if (Constraint == "={ax}")
    return "=a";
  if (Constraint == "{ax}")
    return "a";
  if (Constraint == "{cx}")
    return "c";
  return Constraint;
}

std::string gnuText(const std::string &Text) {
  std::string Out;
  for (const char C : Text) {
    switch (C) {
    case '$':
      Out += '%';
      break;
    case '%':
      Out += "%%";
      break;
    case '\n':
      Out += "\\n";
      break;
    case '\t':
      Out += "\\t";
      break;
    default:
      Out += C;
      break;
    }
  }
  return Out;
}

std::string renderAsm(const X86VirtualizationAsm &Plan,
                      const std::vector<std::string> &Outputs,
                      const std::vector<std::string> &Inputs) {
  if (Outputs.size() != Plan.OutputConstraints.size() ||
      Inputs.size() != Plan.InputConstraints.size())
    throw std::invalid_argument("invalid x86 virtualization assembly operands");
  std::string Text = "__asm__ volatile(\"" + gnuText(Plan.Text) + "\" : ";
  for (size_t I = 0; I < Outputs.size(); ++I) {
    if (I)
      Text += ", ";
    Text += "\"" + gnuConstraint(Plan.OutputConstraints[I]) + "\"(" +
            Outputs[I] + ")";
  }
  Text += " : ";
  for (size_t I = 0; I < Inputs.size(); ++I) {
    if (I)
      Text += ", ";
    Text += "\"" + gnuConstraint(Plan.InputConstraints[I]) + "\"((" +
            unsignedType(Plan.InputBytes[I]) + ")(" + Inputs[I] + "))";
  }
  Text += " : ";
  if (!Outputs.empty())
    Text += "\"cc\", ";
  return Text + "\"memory\");";
}

std::string statusToFlags(const std::string &Status) {
  return "(uint16_t)(((" + Status + " >> 1) & 1u) | ((" + Status +
         " & 1u) << 8))";
}

std::string flagsToStatus(const std::string &Flags) {
  return "(unsigned char)(((" + Flags + " & 1u) << 1) | ((" + Flags +
         " >> 8) & 1u))";
}

std::string nativeBody(const X86VirtualizationNative &Plan,
                       const std::vector<std::string> &Arguments,
                       const std::string &Value, const std::string &Flags,
                       const std::string &Prior = "0",
                       bool PackedFlags = true) {
  const auto C = x86VirtualizationContract(Plan.Assembly.Id);
  std::string Call = Plan.Builtin + "(";
  auto Argument = [&](const std::string &Text) {
    if (Call.back() != '(')
      Call += ", ";
    Call += Text;
  };
  for (size_t I = 0; I < Arguments.size(); ++I) {
    std::string Type = unsignedType(Plan.Assembly.InputBytes[I]);
    if (I == C.MemoryArgument && !Plan.AddressControls)
      Type = Plan.Assembly.Id == Intrinsic::VmxReadMemory
                 ? "unsigned long long *"
             : Plan.Assembly.Id == Intrinsic::VmxPtrStore ? "void *"
                                                          : "const void *";
    Argument("(" + Type + ")(" + Arguments[I] + ")");
  }
  if (Plan.Result == X86VirtualizationResult::Read) {
    Argument(Prior);
    Argument("&__neverd_vmx_status");
  }
  if (Plan.AddressControls) {
    Argument(Plan.Assembly.TargetArch == Arch::X64 ? "64" : "32");
    Argument(std::to_string(Plan.SegmentPrefix));
  }
  Call += "); ";
  std::string Text;
  if (Plan.Result != X86VirtualizationResult::None)
    Text = "unsigned char __neverd_vmx_status; ";
  if (Plan.Result == X86VirtualizationResult::Read)
    Text += Value + " = " + Call;
  else if (Plan.Result == X86VirtualizationResult::Flags)
    Text += "__neverd_vmx_status = " + Call;
  else
    Text += Call;
  if (!Flags.empty())
    Text += Flags + " = " +
            (Plan.Result == X86VirtualizationResult::None ? "0"
             : PackedFlags ? statusToFlags("__neverd_vmx_status")
                           : "__neverd_vmx_status") +
            "; ";
  return Text;
}

std::string renderInstruction(const X86VirtualizationAsm &Plan,
                              NdMemoryAddressSpace Space,
                              const std::vector<std::string> &Outputs,
                              const std::vector<std::string> &Inputs) {
  const auto Assembly = renderAsm(Plan, Outputs, Inputs);
  const auto C = x86VirtualizationContract(Plan.Id);
  const auto Native =
      x86VirtualizationNative(Plan.Id, Plan.TargetArch, Space,
                              C.PhysicalAddress ? Plan.InputBytes[0] : 0);
  // NeverC's native builtin frontend currently targets x86-64 only.
  if (!Native || Plan.TargetArch != Arch::X64)
    return Assembly;
  const bool Read = C.Result == X86VirtualizationResult::Read;
  const auto Body = nativeBody(*Native, Inputs, Read ? Outputs[0] : "",
                               Outputs.empty() ? "" : Outputs[Read ? 1 : 0]);
  return "NEVERD_X86_VIRTUALIZATION(({ " + Body + "}), ({ " + Assembly +
         " }));";
}

bool resultMatches(const llvm::CallInst &Call, Intrinsic Id, uint16_t Word) {
  switch (x86VirtualizationContract(Id).Result) {
  case X86VirtualizationResult::None:
    return Call.getType()->isVoidTy();
  case X86VirtualizationResult::Flags:
    return Call.getType()->isIntegerTy(16);
  case X86VirtualizationResult::Read: {
    const auto *T = llvm::dyn_cast<llvm::StructType>(Call.getType());
    return T && T->isLiteral() && !T->isPacked() && T->getNumElements() == 2 &&
           T->getElementType(0)->isIntegerTy(8 * Word) &&
           T->getElementType(1)->isIntegerTy(16);
  }
  }
  return false;
}

} // namespace

std::string
renderX86VirtualizationExpression(Arch TargetArch, const HighExpr &Expression,
                                  const std::vector<std::string> &Arguments) {
  const auto Id = Expression.IntrinsicId;
  const auto C = x86VirtualizationContract(Id);
  if (!C.Mnemonic)
    return {};
  X86VirtualizationShape Shape;
  Shape.TargetArch = TargetArch;
  Shape.ValidId = Expression.Kind == ExprKind::Call &&
                  Expression.IntrinsicOutputs.empty() &&
                  Expression.Operands.size() == C.Arguments;
  Shape.WritableOutput = true;
  Shape.OutputSize = Expression.Type ? Expression.Type->Size : 0;
  Shape.Arguments = Expression.Operands.size();
  Shape.MemoryOrdering = Expression.MemoryOrdering;
  Shape.MemoryAddressSpace = Expression.MemoryAddressSpace;
  for (size_t I = 0;
       I < Expression.Operands.size() && I < Shape.InputSizes.size(); ++I) {
    const auto &E = Expression.Operands[I];
    Shape.ScalarInputs[I] =
        E && E->Type &&
        (E->Type->Kind == NdTypeKind::Int || E->Type->Kind == NdTypeKind::Ptr);
    Shape.InputSizes[I] = E && E->Type ? E->Type->Size : 0;
    if (I == 1 && E && E->Kind == ExprKind::Const)
      Shape.AddressWidth = E->ConstVal;
    if (I == C.MemoryArgument && E && E->Type &&
        E->Type->Kind == NdTypeKind::Ptr)
      Shape.InputSizes[I] = 8;
  }
  if (!x86VirtualizationShapeIsValid(Id, Shape) ||
      Arguments.size() != C.Arguments)
    throw std::invalid_argument(
        "x86 virtualization expression has an invalid operand/output contract");
  if (Id == Intrinsic::X86SvmRunContext)
    return "__builtin_ia32_vmrun_context((void *)(" + Arguments[0] + "), " +
           std::to_string(*Shape.AddressWidth) + ")";
  const auto Plan =
      x86VirtualizationAsm(Id, TargetArch, Shape.MemoryAddressSpace,
                           C.PhysicalAddress ? Shape.InputSizes[0] : 0);
  if (!Plan)
    throw std::invalid_argument(
        "unsupported x86 virtualization assembly contract");
  if (C.Result == X86VirtualizationResult::None)
    return "({ " +
           renderInstruction(*Plan, Shape.MemoryAddressSpace, {}, Arguments) +
           " })";
  const bool Read = C.Result == X86VirtualizationResult::Read;
  const uint16_t Word = TargetArch == Arch::X64 ? 8 : 4;
  std::string Text = "({ uint16_t __neverd_vmx_flags; ";
  if (Read)
    Text += unsignedType(Word) + " __neverd_vmx_value; ";
  Text +=
      renderInstruction(*Plan, Shape.MemoryAddressSpace,
                        Read ? std::vector<std::string>{"__neverd_vmx_value",
                                                        "__neverd_vmx_flags"}
                             : std::vector<std::string>{"__neverd_vmx_flags"},
                        Arguments);
  if (Read) {
    const auto Wide = unsignedType(2 * Word);
    Text += " (" + Wide +
            ")(__neverd_vmx_flags ? 0 : __neverd_vmx_value) | ((" + Wide +
            ")__neverd_vmx_flags << " + std::to_string(8 * Word) + ")";
  } else
    Text += " __neverd_vmx_flags";
  return Text + "; })";
}

std::optional<std::string>
renderX86VirtualizationCall(Arch TargetArch, const llvm::CallInst &Call,
                            const std::string &Result, bool ResultLive,
                            const std::vector<std::string> &Arguments) {
  const auto *Asm = llvm::dyn_cast<llvm::InlineAsm>(Call.getCalledOperand());
  if (!Asm || (TargetArch != Arch::X86 && TargetArch != Arch::X64))
    return {};
  const uint16_t Word = TargetArch == Arch::X64 ? 8 : 4;
  bool Known = false;
  const auto Mnemonic =
      llvm::StringRef(Asm->getAsmString()).split('\n').first.split(' ').first;
  const Intrinsic Inventory[] = {
#define NEVERD_X86_VIRTUALIZATION(Id, ...) Intrinsic::Id,
#include "neverd/ir/intrinsics/X86/X86Virtualization.def"
#undef NEVERD_X86_VIRTUALIZATION
  };
  for (const auto Id : Inventory) {
    const auto C = x86VirtualizationContract(Id);
    if (Mnemonic != C.Mnemonic)
      continue;
    Known = true;
    for (const auto Space :
         {NdMemoryAddressSpace::Default, NdMemoryAddressSpace::X86FS,
          NdMemoryAddressSpace::X86GS}) {
      const uint16_t PhysicalBytes =
          C.PhysicalAddress && Call.arg_size() &&
                  Call.getArgOperand(0)->getType()->isIntegerTy()
              ? Call.getArgOperand(0)->getType()->getIntegerBitWidth() / 8
              : 0;
      const auto P = x86VirtualizationAsm(Id, TargetArch, Space, PhysicalBytes);
      if (!P || P->Text != Asm->getAsmString() ||
          P->Constraints != Asm->getConstraintString() ||
          !Asm->hasSideEffects() || Asm->isAlignStack() || Asm->canThrow() ||
          Asm->getDialect() != llvm::InlineAsm::AD_ATT ||
          Call.getNumOperandBundles() || Call.doesNotAccessMemory() ||
          Call.arg_size() != P->InputBytes.size() ||
          Arguments.size() != Call.arg_size() || !resultMatches(Call, Id, Word))
        continue;
      bool Types = true;
      for (size_t I = 0; I < P->InputBytes.size(); ++I) {
        const auto *T = Call.getArgOperand(I)->getType();
        if (I == C.MemoryArgument && Space == NdMemoryAddressSpace::Default)
          Types &= T->isPointerTy() && T->getPointerAddressSpace() == 0;
        else
          Types &= T->isIntegerTy(8 * P->InputBytes[I]);
      }
      if (!Types)
        continue;
      if (C.Result == X86VirtualizationResult::None)
        return renderInstruction(*P, Space, {}, Arguments) + '\n';
      const bool Read = C.Result == X86VirtualizationResult::Read;
      if (ResultLive && Result.empty())
        throw std::invalid_argument(
            "x86 virtualization result has no source identity");
      if (ResultLive)
        return renderInstruction(
                   *P, Space,
                   Read ? std::vector<std::string>{Result + ".field_0",
                                                   Result + ".field_1"}
                        : std::vector<std::string>{Result},
                   Arguments) +
               '\n';
      std::string Text = "{ uint16_t __neverd_vmx_flags; ";
      if (Read)
        Text += unsignedType(Word) + " __neverd_vmx_value; ";
      return Text +
             renderInstruction(
                 *P, Space,
                 Read ? std::vector<std::string>{"__neverd_vmx_value",
                                                 "__neverd_vmx_flags"}
                      : std::vector<std::string>{"__neverd_vmx_flags"},
                 Arguments) +
             " }\n";
    }
  }
  if (Known)
    throw std::invalid_argument(
        "unsupported x86 virtualization inline assembly contract");
  return {};
}

bool isX86VirtualizationNativeName(llvm::StringRef Name) {
  if (Name == "llvm.x86.vmrun.context")
    return true;
  if (!Name.consume_front("llvm.x86."))
    return false;
  Name = Name.split('.').first;
  Name.consume_back("32");
  const Intrinsic Inventory[] = {
#define NEVERD_X86_VIRTUALIZATION(Id, ...) Intrinsic::Id,
#include "neverd/ir/intrinsics/X86/X86Virtualization.def"
#undef NEVERD_X86_VIRTUALIZATION
  };
  for (const auto Id : Inventory)
    if (Id != Intrinsic::VmxFunction &&
        Name == x86VirtualizationContract(Id).Mnemonic)
      return true;
  return false;
}

std::optional<std::string>
renderX86VirtualizationNativeCall(Arch TargetArch, const llvm::CallBase &Call,
                                  const std::string &Result, bool ResultLive,
                                  const std::vector<std::string> &Arguments) {
  const auto *Callee = Call.getCalledFunction();
  if (!Callee || !isX86VirtualizationNativeName(Callee->getName()) ||
      (TargetArch != Arch::X86 && TargetArch != Arch::X64))
    return {};
  const auto Name = Callee->getName();
  if (Name == "llvm.x86.vmrun.context") {
    // This extension can be carried by a module read with an older SDK. Its
    // explicit context and conservative call effects are the contract; do not
    // substitute the scalar __svm_vmrun spelling, which loses guest outputs.
    bool Valid =
        TargetArch == Arch::X64 && Callee->isDeclaration() &&
        !Callee->isVarArg() && Call.getCallingConv() == llvm::CallingConv::C &&
        !Call.getNumOperandBundles() && Call.getType()->isVoidTy() &&
        !ResultLive && Call.arg_size() == 2 && Arguments.size() == 2 &&
        Call.getMemoryEffects() == llvm::MemoryEffects::unknown() &&
        !Call.doesNotReturn() && Call.hasFnAttr(llvm::Attribute::Convergent) &&
        Call.hasFnAttr(llvm::Attribute::NoDuplicate);
    const llvm::ConstantInt *Width = nullptr;
    if (Call.arg_size() == 2) {
      const auto *Pointer = Call.getArgOperand(0)->getType();
      Width = llvm::dyn_cast<llvm::ConstantInt>(Call.getArgOperand(1));
      Valid &= Pointer->isPointerTy() &&
               Pointer->getPointerAddressSpace() == 0 && Width &&
               Width->getType()->isIntegerTy(32) &&
               (Width->equalsInt(32) || Width->equalsInt(64));
    }
    if (!Valid)
      throw std::invalid_argument("invalid x64 VMRUN context intrinsic");
    return "__builtin_ia32_vmrun_context((void *)(" + Arguments[0] + "), " +
           std::to_string(Width->getZExtValue()) + ");\n";
  }
  const uint16_t Word = TargetArch == Arch::X64 ? 8 : 4;
  const Intrinsic Inventory[] = {
#define NEVERD_X86_VIRTUALIZATION(Id, ...) Intrinsic::Id,
#include "neverd/ir/intrinsics/X86/X86Virtualization.def"
#undef NEVERD_X86_VIRTUALIZATION
  };
  bool Known = false;
  for (const auto Id : Inventory) {
    if (Id == Intrinsic::VmxFunction)
      continue;
    const auto C = x86VirtualizationContract(Id);
    Known |= Name.starts_with("llvm.x86." + std::string(C.Mnemonic));
    for (const auto Space :
         {NdMemoryAddressSpace::Default, NdMemoryAddressSpace::X86FS,
          NdMemoryAddressSpace::X86GS})
      for (const uint16_t PhysicalBytes : {4, 8}) {
        const auto Plan = x86VirtualizationNative(
            Id, TargetArch, Space, C.PhysicalAddress ? PhysicalBytes : 0);
        if (!Plan || Name != Plan->LLVMName)
          continue;
        const bool Read = Plan->Result == X86VirtualizationResult::Read;
        const bool Status = Plan->Result == X86VirtualizationResult::Flags;
        const size_t Count =
            C.Arguments + (Read ? 1 : 0) + (Plan->AddressControls ? 2 : 0);
        bool Valid = Callee->isIntrinsic() && !Call.getNumOperandBundles() &&
                     !Call.onlyReadsMemory() && !Call.onlyWritesMemory() &&
                     !Call.doesNotReturn() && Call.arg_size() == Count &&
                     Arguments.size() == Count;
        if (Read) {
          const auto *T = llvm::dyn_cast<llvm::StructType>(Call.getType());
          Valid &= T && T->isLiteral() && !T->isPacked() &&
                   T->getNumElements() == 2 &&
                   T->getElementType(0)->isIntegerTy(8 * Word) &&
                   T->getElementType(1)->isIntegerTy(8);
        } else
          Valid &= Status ? Call.getType()->isIntegerTy(8)
                          : Call.getType()->isVoidTy();
        if (!Valid)
          continue;
        for (size_t I = 0; I < C.Arguments; ++I) {
          const auto *T = Call.getArgOperand(I)->getType();
          if (I == C.MemoryArgument && !Plan->AddressControls)
            Valid &= T->isPointerTy() && T->getPointerAddressSpace() == 0;
          else
            Valid &= T->isIntegerTy(8 * Plan->Assembly.InputBytes[I]);
        }
        if (Read)
          Valid &=
              Call.getArgOperand(C.Arguments)->getType()->isIntegerTy(8 * Word);
        if (Plan->AddressControls) {
          const auto *Width = llvm::dyn_cast<llvm::ConstantInt>(
              Call.getArgOperand(C.Arguments));
          const auto *Segment = llvm::dyn_cast<llvm::ConstantInt>(
              Call.getArgOperand(C.Arguments + 1));
          // This projection consumes NeverD's normalized effective address.
          // Other raw native address contracts need their own C projection.
          Valid &= Width && Width->getType()->isIntegerTy(32) &&
                   Width->equalsInt(8 * Word) && Segment &&
                   Segment->getType()->isIntegerTy(32) &&
                   Segment->equalsInt(Plan->SegmentPrefix);
        }
        if (!Valid)
          continue;
        if (ResultLive && Result.empty())
          throw std::invalid_argument(
              "native x86 virtualization result has no source identity");
        const std::vector<std::string> Inputs(Arguments.begin(),
                                              Arguments.begin() + C.Arguments);
        const std::string Prior = Read ? Arguments[C.Arguments] : "0";
        const std::string ValueOutput =
            Read ? ResultLive ? Result + ".field_0" : "__neverd_vmx_discard"
                 : "";
        const std::string StatusOutput = ResultLive ? Read ? Result + ".field_1"
                                                      : Status ? Result
                                                               : ""
                                                    : "";
        std::vector<std::string> Outputs;
        std::string Assembly;
        if (C.Result == X86VirtualizationResult::Read) {
          Assembly += unsignedType(Word) + " __neverd_vmx_value; ";
          Outputs.push_back("__neverd_vmx_value");
        }
        if (C.Result != X86VirtualizationResult::None) {
          Assembly += "uint16_t __neverd_vmx_flags; ";
          Outputs.push_back("__neverd_vmx_flags");
        }
        Assembly += renderAsm(Plan->Assembly, Outputs, Inputs);
        if (Read && ResultLive)
          Assembly += " " + ValueOutput + " = __neverd_vmx_flags ? (" + Prior +
                      ") : __neverd_vmx_value;";
        if (!StatusOutput.empty())
          Assembly += " " + StatusOutput + " = " +
                      flagsToStatus("__neverd_vmx_flags") + ";";
        if (TargetArch == Arch::X86)
          return "{ " + Assembly + " }\n";
        std::string Text = "{ ";
        if (Read && !ResultLive)
          Text += unsignedType(Word) + " __neverd_vmx_discard; ";
        return Text + "NEVERD_X86_VIRTUALIZATION(({ " +
               nativeBody(*Plan, Inputs, ValueOutput, StatusOutput, Prior,
                          false) +
               "}), ({ " + Assembly + " })); }\n";
      }
  }
  if (Known)
    throw std::invalid_argument(
        "unsupported native x86 virtualization C contract");
  return {};
}

} // namespace neverd
