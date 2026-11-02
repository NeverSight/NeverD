//===- HighCPortableX87.cpp - Explicit portable generated C ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep the private portable function interface and exact runtime calls.
//===----------------------------------------------------------------------===//

#include "../HighCWriter.h"

#include "neverd/backend/X86/PortableX87EmissionTarget.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/ir/X86/X87PortableABI.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/med/X86/PortableX87Function.h"

#include "llvm/Support/Errc.h"

#include <exception>
#include <functional>
#include <set>
#include <stdexcept>

namespace neverd {

llvm::Expected<std::string>
HighCEmitter::emitPortableX87(const PreparedPortableX87Function &Prepared,
                              llvm::StringRef OutputTriple) {
  if (auto Error = Prepared.validate())
    return std::move(Error);
  auto Target = portableX87EmissionTarget(OutputTriple);
  if (!Target)
    return Target.takeError();
  try {
    MedToHighConverter Converter;
    std::vector<HighFunc> Functions;
    Functions.push_back(Converter.convert(Prepared.Function, Arch::X64));
    auto &Function = Functions.front();
    if (Function.Params.size() != Prepared.Function.Params.size())
      return llvm::createStringError(llvm::errc::invalid_argument,
                                     "portable x87 lost a fixed parameter");
    for (size_t I = 0; I != Function.Params.size(); ++I) {
      const auto &Original = Prepared.Function.TypedParams[I];
      Function.Params[I].Type = Original.Type;
      if (Function.Params[I].MedIndex != static_cast<int>(I))
        return llvm::createStringError(llvm::errc::invalid_argument,
                                       "portable x87 changed parameter order");
    }
    Function.ReturnType = NdType::makeInt(4, false);
    // Parameter inference must not narrow a guest integer or turn the two
    // explicit host pointers into source-register values.
    std::set<const HighExpr *> Seen;
    std::function<void(const ExprPtr &)> Bind = [&](const ExprPtr &Expr) {
      if (!Expr || !Seen.insert(Expr.get()).second)
        return;
      if (Expr->Kind == ExprKind::Var && Expr->Var.Kind == MedVar::Param) {
        const int Index = Expr->Var.Id;
        if (Index < 0 || static_cast<size_t>(Index) >= Function.Params.size())
          throw std::invalid_argument("portable x87 has an unbound parameter");
        Expr->Type = Function.Params[Index].Type;
      }
      Expr->forEachChildExpr(Bind);
    };
    walkStmts(Function.Body,
              [&](const HighStmt &Statement) { forEachExpr(Statement, Bind); });
    HighCEmitter Emitter;
    Emitter.PortableBody = &Function;
    Emitter.PortableInterface = &Prepared;
    CEmitterOptions Options;
    Options.TheArch = Arch::X64;
    Options.Format = Target->Format;
    Options.EmitComments = false;
    std::string Text;
    llvm::raw_string_ostream Out(Text);
    Out << "#include <stdint.h>\n"
           "_Static_assert(sizeof(void *) == 8, \"portable x87 requires 64-bit "
           "pointers\");\n";
    for (const auto &Signature : x87PortableHelperSignaturesV1())
      Out << Signature.CDeclaration << "\n";
    if (!Emitter.emit(Functions, Out, Options))
      return llvm::createStringError(llvm::errc::invalid_argument,
                                     "portable x87 C emission failed");
    return Text;
  } catch (const std::exception &Error) {
    return llvm::createStringError(llvm::errc::invalid_argument, Error.what());
  }
}

std::string HighCWriter::renderPortableX87Call(const HighExpr &Expr) {
  if (!PortableInterface || !CurrentFunc ||
      CurrentFunc->Name != PortableInterface->name() ||
      !isPortableX87Intrinsic(Expr.IntrinsicId))
    throw std::invalid_argument(
        "portable x87 helper has no prepared C interface");
  const auto &Signature = x87PortableHelperSignatureV1(
      Expr.IntrinsicId == Intrinsic::PortableX87Step
          ? X87PortableHelperV1::Step
          : X87PortableHelperV1::Result);
  if (Expr.Operands.size() != Signature.Parameters.size() ||
      !Expr.Operands[0] || Expr.Operands[0]->Kind != ExprKind::Var ||
      Expr.Operands[0]->Var.Kind != MedVar::Param ||
      Expr.Operands[0]->Var.Id !=
          static_cast<int>(PortableInterface->executionParameterIndex()))
    throw std::invalid_argument("portable x87 helper changed its context role");
  std::string Text = Signature.Name.str() + "(";
  for (size_t I = 0; I != Expr.Operands.size(); ++I) {
    if (!Expr.Operands[I])
      throw std::invalid_argument("portable x87 helper lost an operand");
    if (I)
      Text += ", ";
    const auto Value = Signature.Parameters[I];
    const char *Type = Value == X87PortableABIValueV1::Context ? "void *"
                       : Value == X87PortableABIValueV1::I32   ? "uint32_t"
                                                               : "uint64_t";
    Text += "(" + std::string(Type) + ")(" + exprStr(*Expr.Operands[I]) + ")";
  }
  return Text + ")";
}

} // namespace neverd
