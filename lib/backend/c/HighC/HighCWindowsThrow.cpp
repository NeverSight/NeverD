//===- HighCWindowsThrow.cpp - Windows C++ throw rendering ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Render checked scalar values and rethrows while retaining unknown CRT calls.
//===----------------------------------------------------------------------===//
#include "HighCWriter.h"

#include "neverd/ir/high/MsvcTypeName.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"

namespace neverd {
namespace {
bool isNullThrowArgument(const HighExpr *Value) {
  for (unsigned Depth = 0; Value && Depth != 8; ++Depth) {
    if (Value->Kind == ExprKind::Const)
      return Value->ConstVal == 0;
    if ((Value->Kind == ExprKind::Cast || Value->Kind == ExprKind::BitCast ||
         (Value->Kind == ExprKind::UnaryOp &&
          (Value->Op == NdOp::INT_ZEXT || Value->Op == NdOp::INT_SEXT))) &&
        Value->Operands.size() == 1) {
      Value = Value->Operands[0].get();
      continue;
    }
    return false;
  }
  return false;
}
} // namespace

TypeRef HighCWriter::cxxScalarThrowType(const HighExpr &ThrowCall) const {
  if (!Opts.Image || ThrowCall.Operands.size() != 2 || !ThrowCall.Operands[0] ||
      !ThrowCall.Operands[1])
    return nullptr;
  const auto Address = constAddress(*ThrowCall.Operands[1]);
  const auto Info =
      Address
          ? coff_loader::getCheckedX86SimpleCxxThrowInfo(*Opts.Image, *Address)
          : std::nullopt;
  if (!Info)
    return nullptr;
  const auto Size = Info->TypeDescriptorRange.End - Info->TypeDescriptorVA - 9;
  const auto *Bytes = Opts.Image->readVA(Info->TypeDescriptorVA + 8, Size);
  if (!Bytes)
    return nullptr;
  const llvm::StringRef Name(reinterpret_cast<const char *>(Bytes), Size);
  const auto Type = msvc_type_name::fundamental(Name);
  if (!Type || Info->ObjectSize != Type->Size)
    return nullptr;
  auto Result = Type->Floating ? NdType::makeFloat(Type->Size)
                               : NdType::makeInt(Type->Size, Type->Signed);
  Result->SourceName = Type->Spelling.str();
  return Result;
}

void HighCWriter::writeCxxThrowExpr(const HighStmt &Stmt,
                                    const HighExpr &ThrowCall) {
  const bool NullObject = !ThrowCall.Operands.empty() &&
                          isNullThrowArgument(ThrowCall.Operands[0].get());
  const bool Rethrow = ThrowCall.Operands.size() == 2 && NullObject &&
                       isNullThrowArgument(ThrowCall.Operands[1].get());
  if (!Opts.StructuredExceptionSyntax || ThrowCall.Operands.empty() ||
      (NullObject && !Rethrow)) {
    OS << exprStr(ThrowCall);
    return;
  }
  if (!NullObject)
    if (auto Type = cxxScalarThrowType(ThrowCall)) {
      // Read the current object bytes, including floating-point bits, rather
      // than treating its address as the value or inventing a constructor.
      OS << "throw (" << Type->SourceName << ")"
         << memoryLoadExpr(Type, addrStr(*ThrowCall.Operands[0]));
      return;
    }
  if (Opts.Image && Opts.Image->Arch == Arch::X86 && !Rethrow &&
      !CxxThrowPrints.count(&Stmt)) {
    OS << exprStr(ThrowCall);
    return;
  }
  OS << "throw";
  if (auto Printed = CxxThrowPrints.find(&Stmt);
      Printed != CxxThrowPrints.end()) {
    OS << " " << Printed->second.Type << "(";
    for (size_t I = 0; I < Printed->second.Args.size(); ++I) {
      if (I)
        OS << ", ";
      OS << exprStr(*Printed->second.Args[I]);
    }
    OS << ")";
    return;
  }
  if (!ThrowCall.Operands.empty() && ThrowCall.Operands[0] && !Rethrow)
    OS << " " << exprStr(*ThrowCall.Operands[0]);
}

} // namespace neverd
