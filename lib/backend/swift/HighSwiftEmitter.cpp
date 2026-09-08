//===- HighSwiftEmitter.cpp - Structured HighIR to Swift source
//------------===//
#include "neverd/backend/swift/HighSwiftEmitter.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>

namespace neverd {
namespace {

struct Unsupported : std::runtime_error {
  using std::runtime_error::runtime_error;
};

bool identifier(const std::string &Text) {
  if (Text.empty() ||
      !(std::isalpha(static_cast<unsigned char>(Text[0])) || Text[0] == '_'))
    return false;
  return std::all_of(Text.begin(), Text.end(), [](unsigned char C) {
    return C < 128 && (std::isalnum(C) || C == '_');
  });
}

std::string name(const std::string &Text) {
  if (!identifier(Text) || Text == "self" || Text == "Self" ||
      Text == "super" || Text == "Swift")
    throw Unsupported("unsafe or reserved Swift declaration identifier");
  return "`" + Text + "`";
}

unsigned width(const TypeRef &Type) {
  if (Type && Type->Kind == NdTypeKind::Int && Type->Size == 16)
    return 128;
  if (!Type ||
      (Type->Kind != NdTypeKind::Int && Type->Kind != NdTypeKind::Ptr &&
       Type->Kind != NdTypeKind::Float) ||
      (Type->Size != 1 && Type->Size != 2 && Type->Size != 4 &&
       Type->Size != 8))
    throw Unsupported("unsupported or missing machine scalar type");
  if (Type->Kind == NdTypeKind::Float && Type->Size != 4 && Type->Size != 8)
    throw Unsupported("unsupported floating-point representation");
  return Type->Size * 8;
}

std::string uintType(unsigned Bits) {
  if (Bits > 64)
    throw Unsupported(
        "wide Swift containers require an explicit lane operation");
  return "Swift.UInt" + std::to_string(Bits);
}
std::string intType(unsigned Bits) {
  if (Bits > 64)
    throw Unsupported(
        "wide Swift containers have no implicit signed scalar interpretation");
  return "Swift.Int" + std::to_string(Bits);
}

std::string floatType(unsigned Bits) {
  if (Bits == 32)
    return "Swift.Float";
  if (Bits == 64)
    return "Swift.Double";
  throw Unsupported("unsupported Swift floating-point width");
}

std::string floatValue(const std::string &Bits, unsigned Width) {
  return floatType(Width) + "(bitPattern: " + uintType(Width) +
         "(truncatingIfNeeded: " + Bits + "))";
}

std::string floatBits(const std::string &Value) {
  return "Swift.UInt64((" + Value + ").bitPattern)";
}

std::string narrowed(const std::string &Value, unsigned Bits) {
  if (Bits == 64)
    return Value;
  return "Swift.UInt64(" + uintType(Bits) + "(truncatingIfNeeded: " + Value +
         "))";
}

std::string signedValue(const std::string &Value, unsigned Bits) {
  return intType(Bits) + "(bitPattern: " + uintType(Bits) +
         "(truncatingIfNeeded: " + Value + "))";
}

std::string swiftType(const SwiftSourceType &Type, unsigned Depth = 0) {
  if (Depth > 8)
    throw Unsupported("Swift pointer type is too deep");
  switch (Type.TheKind) {
  case SwiftSourceType::Kind::Void:
    if (Type.Name == "Void")
      return "Swift.Void";
    break;
  case SwiftSourceType::Kind::Boolean:
    if (Type.Name == "Bool" && Type.Bits == 1)
      return "Swift.Bool";
    break;
  case SwiftSourceType::Kind::Integer: {
    const std::string Fixed =
        (Type.IsSigned ? "Int" : "UInt") + std::to_string(Type.Bits);
    const std::string Word = Type.IsSigned ? "Int" : "UInt";
    if ((Type.Bits == 8 || Type.Bits == 16 || Type.Bits == 32 ||
         Type.Bits == 64) &&
        (Type.Name == Fixed || (Type.Bits == 64 && Type.Name == Word)))
      return "Swift." + Type.Name;
    break;
  }
  case SwiftSourceType::Kind::Floating:
    if ((Type.Name == "Float" && Type.Bits == 32) ||
        (Type.Name == "Double" && Type.Bits == 64))
      return "Swift." + Type.Name;
    break;
  case SwiftSourceType::Kind::Pointer:
    if ((Type.Name == "UnsafeRawPointer" ||
         Type.Name == "UnsafeMutableRawPointer") &&
        !Type.Pointee)
      return "Swift." + Type.Name;
    if ((Type.Name == "UnsafePointer" || Type.Name == "UnsafeMutablePointer") &&
        Type.Pointee && Type.Pointee->TheKind != SwiftSourceType::Kind::Void)
      return "Swift." + Type.Name + "<" + swiftType(*Type.Pointee, Depth + 1) +
             ">";
    break;
  }
  throw Unsupported("unsupported Swift source type");
}

bool typeMatches(const TypeRef &Native, const SwiftSourceType &Source) {
  if (!Native)
    return false;
  switch (Source.TheKind) {
  case SwiftSourceType::Kind::Void:
    return Native->Kind == NdTypeKind::Void;
  case SwiftSourceType::Kind::Floating:
    return Native->Kind == NdTypeKind::Float && Native->Size * 8 == Source.Bits;
  case SwiftSourceType::Kind::Pointer:
    return Native->Kind == NdTypeKind::Ptr && Native->Size == 8;
  case SwiftSourceType::Kind::Boolean:
    return Native->Kind == NdTypeKind::Int && Native->Size == 1;
  case SwiftSourceType::Kind::Integer:
    return Native->Kind == NdTypeKind::Int && Native->Size * 8 == Source.Bits &&
           Native->IsSigned == Source.IsSigned;
  }
  return false;
}

bool carrierMatches(const TypeRef &Native, const SwiftSourceType &Source) {
  if (!Native)
    return false;
  const unsigned Bytes = Source.TheKind == SwiftSourceType::Kind::Pointer ? 8
                         : Source.TheKind == SwiftSourceType::Kind::Boolean
                             ? 1
                             : Source.Bits / 8;
  return Bytes && Native->Size == Bytes &&
         (Native->Kind == NdTypeKind::Int || typeMatches(Native, Source));
}

std::string sourceToBits(const std::string &Value,
                         const SwiftSourceType &Type) {
  if (Type.TheKind == SwiftSourceType::Kind::Floating)
    return floatBits(Value);
  if (Type.TheKind == SwiftSourceType::Kind::Boolean)
    return "(" + Value + " ? Swift.UInt64(1) : Swift.UInt64(0))";
  if (Type.TheKind == SwiftSourceType::Kind::Pointer)
    return "Swift.UInt64(Swift.UInt(bitPattern: " + Value + "))";
  return "Swift.UInt64(truncatingIfNeeded: " + Value + ")";
}

std::string bitsToSource(const std::string &Value,
                         const SwiftSourceType &Type) {
  const std::string Spelling = swiftType(Type);
  if (Type.TheKind == SwiftSourceType::Kind::Floating)
    return floatValue(Value, Type.Bits);
  if (Type.TheKind == SwiftSourceType::Kind::Boolean)
    return "(" + Value + " != 0)";
  if (Type.TheKind == SwiftSourceType::Kind::Pointer)
    return Spelling + "(bitPattern: Swift.UInt(" + Value + "))!";
  return Spelling + "(truncatingIfNeeded: " + Value + ")";
}

class Writer {
  const HighFunc &F;
  const SwiftSourceSignature &S;
  const std::vector<SwiftSourceSignature> &Callees;
  std::set<va_t> Dependencies;
  std::map<VarKey, std::string> Variables;
  std::map<VarKey, unsigned> VariableWidths;
  std::set<VarKey> Defined;
  std::set<std::string> Initialized;
  std::string Statements;
  std::string Member;
  bool Initializer = false;
  bool UsesWide = false;
  std::string WideType = "nd_word128";
  unsigned Indent = 1;
  unsigned LoopDepth = 0;
  unsigned SwitchDepth = 0;
  unsigned Count = 0;
  struct FlowState {
    std::set<VarKey> Locals;
    std::set<std::string> Fields;
  };
  struct ControlScope {
    bool Loop = false;
    std::optional<FlowState> BreakExits;
    std::optional<FlowState> ContinueEdges;
  };
  std::vector<ControlScope> Controls;
  bool FallsThrough = true;

  FlowState flowState() const { return {Defined, Initialized}; }
  void restoreFlow(const FlowState &State) {
    Defined = State.Locals;
    Initialized = State.Fields;
    FallsThrough = true;
  }
  static void mergeFlow(std::optional<FlowState> &Paths,
                        const FlowState &Next) {
    if (!Paths) {
      Paths = Next;
      return;
    }
    FlowState Common;
    std::set_intersection(Paths->Locals.begin(), Paths->Locals.end(),
                          Next.Locals.begin(), Next.Locals.end(),
                          std::inserter(Common.Locals, Common.Locals.begin()));
    std::set_intersection(Paths->Fields.begin(), Paths->Fields.end(),
                          Next.Fields.begin(), Next.Fields.end(),
                          std::inserter(Common.Fields, Common.Fields.begin()));
    Paths = std::move(Common);
  }
  void finishFlow(const std::optional<FlowState> &Exits,
                  const FlowState &Before) {
    restoreFlow(Exits ? *Exits : Before);
    FallsThrough = Exits.has_value();
  }
  static std::optional<bool> constantCondition(const ExprPtr &E) {
    if (!E || E->Kind != ExprKind::Const || !E->Operands.empty() || !E->Type ||
        (E->Type->Kind != NdTypeKind::Int && E->Type->Kind != NdTypeKind::Ptr))
      return std::nullopt;
    const unsigned Bits = width(E->Type);
    if (Bits > 64)
      throw Unsupported("Swift condition has no scalar truth value");
    const uint64_t Mask = Bits == 64 ? UINT64_MAX : (UINT64_C(1) << Bits) - 1;
    return (E->ConstVal & Mask) != 0;
  }
  std::string condition(const ExprPtr &E) {
    if (auto Known = constantCondition(E))
      return *Known ? "true" : "false";
    return expression(E) + " != 0";
  }

  bool isSelf(const ExprPtr &E, unsigned Depth = 0) const {
    if (!E || Depth > 200 || S.IsStatic ||
        (S.ContextKind != "class" &&
         !(S.ContextKind == "struct" &&
           S.SelfConvention == "indirect-mutating")))
      return false;
    if ((E->Kind == ExprKind::Cast || E->Kind == ExprKind::BitCast) &&
        E->Operands.size() == 1 && E->Type && E->Operands[0] &&
        E->Operands[0]->Type && E->Type->Size == 8 &&
        E->Operands[0]->Type->Size == 8)
      return isSelf(E->Operands[0], Depth + 1);
    return E->Kind == ExprKind::Var && E->Var.Kind == MedVar::Param &&
           E->Var.RenameTag < 0 && E->Var.Id >= 0 &&
           static_cast<size_t>(E->Var.Id) == S.Parameters.size();
  }

  bool containsSelf(const ExprPtr &E, unsigned Depth = 0) const {
    if (!E || Depth > 200)
      return true;
    if (isSelf(E))
      return true;
    for (const auto &Operand : E->Operands)
      if (containsSelf(Operand, Depth + 1))
        return true;
    return false;
  }

  const SwiftStorageField *field(const ExprPtr &Address, unsigned Bytes) const {
    uint64_t Offset = 0;
    if (!isSelf(Address)) {
      if (!Address || Address->Kind != ExprKind::BinOp ||
          Address->Op != NdOp::INT_ADD || Address->Operands.size() != 2)
        return nullptr;
      ExprPtr Constant;
      if (isSelf(Address->Operands[0]))
        Constant = Address->Operands[1];
      else if (isSelf(Address->Operands[1]))
        Constant = Address->Operands[0];
      if (!Constant || Constant->Kind != ExprKind::Const)
        return nullptr;
      Offset = Constant->ConstVal;
    }
    if (!S.ContextLayoutKnown)
      return nullptr;
    for (const auto &Field : S.ContextFields) {
      const auto &Type = Field.Type;
      const unsigned Size = Type.TheKind == SwiftSourceType::Kind::Pointer ? 8
                            : Type.TheKind == SwiftSourceType::Kind::Boolean
                                ? 1
                                : Type.Bits / 8;
      if (Field.Offset == Offset && Size == Bytes)
        return &Field;
    }
    return nullptr;
  }

  void requireInitialized() const {
    for (const auto &Field : S.ContextFields)
      if (!Initialized.count(Field.Name))
        throw Unsupported("Swift initializer leaves a stored property "
                          "uninitialized on a return path");
  }

  std::string fieldName(const SwiftStorageField &Field) const {
    return name(Field.BackingName.empty() ? Field.Name : Field.BackingName);
  }

  std::string variable(const MedVar &V, unsigned Bits) {
    if (V.Kind == MedVar::Param && V.RenameTag < 0) {
      if (V.Id < 0 || static_cast<size_t>(V.Id) >= F.Params.size())
        throw Unsupported("unbound Swift source parameter");
      return "nd_arg" + std::to_string(V.Id);
    }
    const VarKey Key = varKey(V);
    auto [Width, New] = VariableWidths.emplace(Key, Bits);
    if (!New && Width->second != Bits)
      throw Unsupported("Swift local value has inconsistent machine widths");
    auto [It, Inserted] =
        Variables.emplace(Key, "nd_v" + std::to_string(Variables.size()));
    return It->second;
  }

  std::string wideExpression(const ExprPtr &E, unsigned Depth) {
    UsesWide = true;
    auto Pair = [&](const std::string &Low, const std::string &High) {
      return WideType + "(lo: " + Low + ", hi: " + High + ")";
    };
    switch (E->Kind) {
    case ExprKind::Const:
      return Pair("Swift.UInt64(" + std::to_string(E->ConstVal) + ")", "0");
    case ExprKind::Var:
      if (E->Var.Kind == MedVar::Param || !Defined.count(varKey(E->Var)))
        throw Unsupported("wide Swift value is not a defined local container");
      return variable(E->Var, 128);
    case ExprKind::BinOp:
      if (E->Op == NdOp::CONCAT && E->Operands.size() == 2 && E->Operands[0] &&
          E->Operands[1] && width(E->Operands[0]->Type) == 64 &&
          width(E->Operands[1]->Type) == 64)
        return Pair(expression(E->Operands[1], Depth + 1),
                    expression(E->Operands[0], Depth + 1));
      break;
    case ExprKind::Load:
      if (E->Operands.size() == 1 && E->Operands[0] &&
          E->MemoryAddressSpace == NdMemoryAddressSpace::Default &&
          E->MemoryOrdering == NdMemoryOrdering::None &&
          width(E->Operands[0]->Type) == 64 && !containsSelf(E->Operands[0]))
        return WideType +
               ".load(Swift.UnsafeRawPointer(bitPattern: Swift.UInt(" +
               expression(E->Operands[0], Depth + 1) + "))!)";
      break;
    case ExprKind::Cast:
    case ExprKind::UnaryOp:
      if (E->Operands.size() == 1 && E->Operands[0] &&
          ((E->Kind == ExprKind::Cast && E->CastTo &&
            E->CastTo->Kind == NdTypeKind::Int && E->CastTo->Size == 16) ||
           E->Op == NdOp::INT_ZEXT || E->Op == NdOp::INT_SEXT)) {
        const auto &Operand = E->Operands[0];
        const unsigned Bits = width(Operand->Type);
        if (Bits > 64 || Operand->Type->Kind != NdTypeKind::Int)
          break;
        const auto Value = expression(Operand, Depth + 1);
        const bool Signed = E->Kind == ExprKind::Cast ? Operand->Type->IsSigned
                                                      : E->Op == NdOp::INT_SEXT;
        if (!Signed)
          return Pair(Value, "0");
        const auto Sign = signedValue(Value, Bits);
        return Pair("Swift.UInt64(truncatingIfNeeded: " + Sign + ")",
                    "(" + Sign + " < 0 ? Swift.UInt64.max : 0)");
      }
      break;
    default:
      break;
    }
    throw Unsupported("wide Swift container requires a proven concatenation, "
                      "extension, or ordinary memory operation");
  }

  std::string wideHelper() const {
    if (!UsesWide)
      return {};
    return "    struct " + WideType +
           " {\n"
           "        var lo: Swift.UInt64\n        var hi: Swift.UInt64\n"
           "        func slice(_ bytes: Swift.UInt64) -> Swift.UInt64 {\n"
           "            if bytes == 0 { return lo }\n"
           "            if bytes < 8 { return (lo >> (bytes * 8)) | (hi << ((8 "
           "- bytes) * 8)) }\n"
           "            return hi >> ((bytes - 8) * 8)\n        }\n"
           "        static func load(_ p: Swift.UnsafeRawPointer) -> Self {\n"
           "            return Self(lo: p.loadUnaligned(as: "
           "Swift.UInt64.self), hi: p.advanced(by: 8).loadUnaligned(as: "
           "Swift.UInt64.self))\n        }\n"
           "        func store(_ p: Swift.UnsafeMutableRawPointer) {\n"
           "            p.storeBytes(of: lo, as: Swift.UInt64.self)\n"
           "            p.advanced(by: 8).storeBytes(of: hi, as: "
           "Swift.UInt64.self)\n        }\n    }\n";
  }

  std::string call(const ExprPtr &E, unsigned Depth, bool Statement = false) {
    if (!E || E->Kind != ExprKind::Call || E->IsIndirectCall ||
        E->IntrinsicId != Intrinsic::None ||
        E->MemoryAddressSpace != NdMemoryAddressSpace::Default)
      throw Unsupported("Swift call has no ordinary direct source binding");
    const SwiftSourceSignature *Target = nullptr;
    for (const auto &Candidate : Callees) {
      if (Candidate.Entry != E->CallAddr)
        continue;
      if (Target)
        throw Unsupported("Swift call target has ambiguous source signatures");
      Target = &Candidate;
    }
    const bool SameSelf = Target && S.ContextKind == "class" &&
                          Target->ContextKind == "class" && !S.IsStatic &&
                          !Target->IsStatic &&
                          S.ContextName == Target->ContextName;
    const bool Getter = Target && Target->DeclarationKind == "getter";
    const bool Setter = Target && Target->DeclarationKind == "setter";
    if (!Target || !Target->UnsupportedReason.empty() ||
        Target->Module != S.Module ||
        (Target->DeclarationKind != "function" && !Getter && !Setter) ||
        (Getter &&
         (!Target->Parameters.empty() ||
          Target->ReturnType.TheKind == SwiftSourceType::Kind::Void)) ||
        (Setter &&
         (Target->Parameters.size() != 1 ||
          Target->ReturnType.TheKind != SwiftSourceType::Kind::Void)) ||
        (Target->ContextKind != "global" &&
         !(Target->ContextKind == "struct" && Target->IsStatic) && !SameSelf) ||
        E->Operands.size() != Target->Parameters.size() + size_t(SameSelf) ||
        Target->Labels.size() != Target->Parameters.size())
      throw Unsupported("Swift call target or argument binding is unavailable");
    if (SameSelf) {
      const auto &Receiver = E->Operands.back();
      if (!isSelf(Receiver) || !Receiver->Type ||
          Receiver->Type->Kind != NdTypeKind::Ptr || Receiver->Type->Size != 8)
        throw Unsupported(
            "Swift direct instance call does not preserve its bound receiver");
      if (Initializer)
        requireInitialized();
    }
    // Source-bound calls retain their raw machine return bits in HighIR. The
    // declaration comes from the complete source call binding, not the signed
    // interpretation of that integer register container.
    const auto &Binding = E->SourceCallHint;
    bool Bound = Binding &&
                 Binding->CallKind == SourceCallTypeHint::Kind::Native &&
                 Binding->TargetAddress == E->CallAddr &&
                 Binding->Signature.HasExplicitABI &&
                 Binding->Signature.Parameters.size() == E->Operands.size() &&
                 typeMatches(Binding->Signature.ReturnType, Target->ReturnType);
    if (Bound) {
      for (size_t Index = 0; Index < Target->Parameters.size(); ++Index)
        Bound &= typeMatches(Binding->Signature.Parameters[Index].Type,
                             Target->Parameters[Index].Type);
      if (SameSelf) {
        const auto &Receiver = Binding->Signature.Parameters.back().Type;
        Bound &= Receiver && Receiver->Kind == NdTypeKind::Ptr &&
                 Receiver->Size == 8;
      }
    }
    if (Binding && !Bound)
      throw Unsupported(
          "Swift call source binding disagrees with its declaration");
    if (!typeMatches(E->Type, Target->ReturnType) &&
        !(Bound && ((Statement && Target->ReturnType.TheKind ==
                                      SwiftSourceType::Kind::Void) ||
                    carrierMatches(E->Type, Target->ReturnType))))
      throw Unsupported(
          "Swift call result disagrees with the callee signature");
    std::string Text = SameSelf ? "self."
                       : Target->ContextKind == "global"
                           ? ""
                           : name(Target->ContextName) + ".";
    Text += name(Target->Name) + (Getter ? "" : Setter ? " = " : "(");
    for (size_t Index = 0; Index < Target->Parameters.size(); ++Index) {
      if (!E->Operands[Index] ||
          (!typeMatches(E->Operands[Index]->Type,
                        Target->Parameters[Index].Type) &&
           !(Bound && carrierMatches(E->Operands[Index]->Type,
                                     Target->Parameters[Index].Type))))
        throw Unsupported(
            "Swift call operand disagrees with the callee signature");
      if (Index)
        Text += ", ";
      if (!Setter && Target->Labels[Index] != "_")
        Text += name(Target->Labels[Index]) + ": ";
      Text += bitsToSource(expression(E->Operands[Index], Depth + 1),
                           Target->Parameters[Index].Type);
    }
    if (!Getter && !Setter)
      Text += ")";
    Dependencies.insert(Target->Entry);
    if (Statement)
      return Target->ReturnType.TheKind == SwiftSourceType::Kind::Void
                 ? Text
                 : "_ = " + Text;
    if (Target->ReturnType.TheKind == SwiftSourceType::Kind::Void)
      throw Unsupported("void Swift call used as a value");
    return sourceToBits(Text, Target->ReturnType);
  }

  std::string expression(const ExprPtr &E, unsigned Depth = 0) {
    if (!E || Depth > 200)
      throw Unsupported("missing or excessively deep Swift source expression");
    if (E->Kind == ExprKind::Call)
      return call(E, Depth);
    const unsigned Bits = width(E->Type);
    if (Bits == 128)
      return wideExpression(E, Depth);
    auto Operand = [&](size_t Index) {
      if (Index >= E->Operands.size())
        throw Unsupported("missing Swift expression operand");
      return expression(E->Operands[Index], Depth + 1);
    };
    switch (E->Kind) {
    case ExprKind::Const:
      return narrowed("Swift.UInt64(" + std::to_string(E->ConstVal) + ")",
                      Bits);
    case ExprKind::Var: {
      if (isSelf(E))
        throw Unsupported("Swift body uses raw self outside a proven stored "
                          "property projection");
      if (!(E->Var.Kind == MedVar::Param && E->Var.RenameTag < 0) &&
          !Defined.count(varKey(E->Var)))
        throw Unsupported(
            "method reads an unbound register or undefined local");
      return narrowed(variable(E->Var, Bits), Bits);
    }
    case ExprKind::BitCast:
      // Every internal scalar is represented by its raw UInt64 bit pattern.
      // No numeric conversion is performed; FP arithmetic reinterprets it
      // explicitly using Float/Double(bitPattern:) at the operation boundary.
      if (E->Operands.size() != 1 || !E->Operands[0] ||
          Bits != width(E->Operands[0]->Type))
        throw Unsupported("Swift bitcast changes the machine value width");
      return Operand(0);
    case ExprKind::Cast:
      if (!E->CastTo || E->Operands.size() != 1 || !E->Operands[0] ||
          !E->Operands[0]->Type)
        throw Unsupported("unsupported Swift scalar cast");
      if (E->CastTo->Kind == NdTypeKind::Float ||
          E->Operands[0]->Type->Kind == NdTypeKind::Float)
        throw Unsupported("floating-point numeric casts require an explicit "
                          "conversion operation");
      if (width(E->Operands[0]->Type) == 128)
        return narrowed("(" + Operand(0) + ").lo", Bits);
      if (width(E->CastTo) > width(E->Operands[0]->Type) &&
          E->Operands[0]->Type->IsSigned)
        return narrowed(
            "Swift.UInt64(truncatingIfNeeded: " +
                signedValue(Operand(0), width(E->Operands[0]->Type)) + ")",
            width(E->CastTo));
      return narrowed(Operand(0), width(E->CastTo));
    case ExprKind::UnaryOp: {
      if (E->Operands.size() != 1 || !E->Operands[0])
        throw Unsupported("unsupported unary operand count");
      if (width(E->Operands[0]->Type) > 64)
        throw Unsupported("Swift scalar unary operation requires an explicit "
                          "wide lane extraction");
      const std::string A = Operand(0);
      switch (E->Op) {
      case NdOp::FLOAT_NEG:
        return "(" + A + " ^ Swift.UInt64(" +
               std::to_string(uint64_t(1) << (Bits - 1)) + "))";
      case NdOp::FLOAT_ABS:
        return "(" + A + " & Swift.UInt64(" +
               std::to_string((uint64_t(1) << (Bits - 1)) - 1) + "))";
      case NdOp::FLOAT_ISNAN:
        return "(" + floatValue(A, width(E->Operands[0]->Type)) +
               ".isNaN ? Swift.UInt64(1) : Swift.UInt64(0))";
      case NdOp::FLOAT_SQRT:
        return floatBits(floatValue(A, Bits) + ".squareRoot()");
      case NdOp::FLOAT_INT2FLOAT:
        return floatBits(floatType(Bits) + "(" +
                         signedValue(A, width(E->Operands[0]->Type)) + ")");
      case NdOp::FLOAT_UINT2FLOAT:
        return floatBits(floatType(Bits) + "(" + A + ")");
      case NdOp::FLOAT_FLOAT2FLOAT:
        return floatBits(floatType(Bits) + "(" +
                         floatValue(A, width(E->Operands[0]->Type)) + ")");
      case NdOp::INT_NEGATE:
      case NdOp::INT_NOT:
        return narrowed("(~(" + A + "))", Bits);
      case NdOp::INT_NEG2:
        return narrowed("(Swift.UInt64(0) &- " + A + ")", Bits);
      case NdOp::BOOL_NOT:
        return "(" + A + " == 0 ? Swift.UInt64(1) : Swift.UInt64(0))";
      case NdOp::INT_ZEXT:
        return narrowed(A, Bits);
      case NdOp::INT_SEXT:
        return narrowed("Swift.UInt64(truncatingIfNeeded: " +
                            signedValue(A, width(E->Operands[0]->Type)) + ")",
                        Bits);
      default:
        throw Unsupported("unsupported Swift unary operation");
      }
    }
    case ExprKind::BinOp: {
      if (E->Op == NdOp::SUBBYTES && E->Operands.size() == 2 &&
          E->Operands[0] && E->Operands[1] &&
          E->Operands[1]->Kind == ExprKind::Const) {
        const unsigned SourceBits = width(E->Operands[0]->Type);
        const auto Offset = E->Operands[1]->ConstVal;
        if (Offset > SourceBits / 8 || Bits / 8 > SourceBits / 8 - Offset)
          throw Unsupported(
              "Swift byte extraction exceeds its defined container");
        return narrowed(
            SourceBits == 128
                ? "(" + Operand(0) + ").slice(" + std::to_string(Offset) + ")"
                : "(" + Operand(0) + " >> " + std::to_string(Offset * 8) + ")",
            Bits);
      }
      if (E->Op == NdOp::CONCAT && E->Operands.size() == 2 && E->Operands[0] &&
          E->Operands[1]) {
        const auto HighBits = width(E->Operands[0]->Type),
                   LowBits = width(E->Operands[1]->Type);
        if (HighBits + LowBits != Bits || LowBits >= 64)
          throw Unsupported(
              "Swift concatenation has inconsistent container widths");
        return narrowed("((" + Operand(0) + " << " + std::to_string(LowBits) +
                            ") | " + Operand(1) + ")",
                        Bits);
      }
      for (const auto &Input : E->Operands)
        if (!Input || width(Input->Type) > 64)
          throw Unsupported("Swift scalar arithmetic requires an explicit wide "
                            "lane extraction");
      if (E->Op == NdOp::SELECT && E->Operands.size() == 3)
        return narrowed("(" + Operand(0) + " != 0 ? " + Operand(1) + " : " +
                            Operand(2) + ")",
                        Bits);
      if (E->Operands.size() != 2)
        throw Unsupported("unsupported binary operand count");
      std::string A = Operand(0), B = Operand(1), Op;
      bool Boolean = false;
      switch (E->Op) {
      case NdOp::FLOAT_ADD:
      case NdOp::FLOAT_SUB:
      case NdOp::FLOAT_MULT:
      case NdOp::FLOAT_DIV:
        Op = E->Op == NdOp::FLOAT_ADD    ? "+"
             : E->Op == NdOp::FLOAT_SUB  ? "-"
             : E->Op == NdOp::FLOAT_MULT ? "*"
                                         : "/";
        return floatBits("(" + floatValue(A, width(E->Operands[0]->Type)) +
                         " " + Op + " " +
                         floatValue(B, width(E->Operands[1]->Type)) + ")");
      case NdOp::FLOAT_EQUAL:
      case NdOp::FLOAT_NOTEQUAL:
      case NdOp::FLOAT_LESS:
      case NdOp::FLOAT_LESSEQUAL:
        A = floatValue(A, width(E->Operands[0]->Type));
        B = floatValue(B, width(E->Operands[1]->Type));
        Op = E->Op == NdOp::FLOAT_EQUAL      ? "=="
             : E->Op == NdOp::FLOAT_NOTEQUAL ? "!="
             : E->Op == NdOp::FLOAT_LESS     ? "<"
                                             : "<=";
        Boolean = true;
        break;
      case NdOp::INT_ADD:
        Op = "&+";
        break;
      case NdOp::INT_SUB:
        Op = "&-";
        break;
      case NdOp::INT_MULT:
        Op = "&*";
        break;
      case NdOp::INT_AND:
        Op = "&";
        break;
      case NdOp::INT_OR:
        Op = "|";
        break;
      case NdOp::INT_XOR:
        Op = "^";
        break;
      case NdOp::INT_LEFT:
        Op = "<<";
        break;
      case NdOp::INT_RIGHT:
        Op = ">>";
        break;
      case NdOp::INT_ASHR:
        A = signedValue(A, width(E->Operands[0]->Type));
        return narrowed(
            "Swift.UInt64(truncatingIfNeeded: " + A + " >> " + B + ")", Bits);
      case NdOp::INT_EQUAL:
        Op = "==";
        Boolean = true;
        break;
      case NdOp::INT_NOTEQUAL:
        Op = "!=";
        Boolean = true;
        break;
      case NdOp::INT_LESS:
        Op = "<";
        Boolean = true;
        break;
      case NdOp::INT_LESSEQUAL:
        Op = "<=";
        Boolean = true;
        break;
      case NdOp::INT_SLESS:
      case NdOp::INT_SLESSEQUAL:
        A = signedValue(A, width(E->Operands[0]->Type));
        B = signedValue(B, width(E->Operands[1]->Type));
        Op = E->Op == NdOp::INT_SLESS ? "<" : "<=";
        Boolean = true;
        break;
      case NdOp::BOOL_AND:
        A = "(" + A + " != 0)";
        B = "(" + B + " != 0)";
        Op = "&&";
        Boolean = true;
        break;
      case NdOp::BOOL_OR:
        A = "(" + A + " != 0)";
        B = "(" + B + " != 0)";
        Op = "||";
        Boolean = true;
        break;
      case NdOp::BOOL_XOR:
        A = "(" + A + " != 0)";
        B = "(" + B + " != 0)";
        Op = "!=";
        Boolean = true;
        break;
      default:
        throw Unsupported("unsupported Swift binary operation");
      }
      const std::string Value = "(" + A + " " + Op + " " + B + ")";
      return Boolean ? "(" + Value + " ? Swift.UInt64(1) : Swift.UInt64(0))"
                     : narrowed(Value, Bits);
    }
    case ExprKind::Load:
      if (E->Operands.size() != 1 ||
          E->MemoryAddressSpace != NdMemoryAddressSpace::Default ||
          E->MemoryOrdering != NdMemoryOrdering::None)
        throw Unsupported(
            "unsupported ordered or non-default Swift memory read");
      if (const auto *Field = field(E->Operands[0], Bits / 8)) {
        if (Initializer && !Initialized.count(Field->Name))
          throw Unsupported("Swift initializer reads a stored property before "
                            "initialization");
        return narrowed(sourceToBits("self." + fieldName(*Field), Field->Type),
                        Bits);
      }
      if (containsSelf(E->Operands[0]))
        throw Unsupported(
            "Swift self memory read does not match a proven stored property");
      return "Swift.UInt64(Swift.UnsafeRawPointer(bitPattern: Swift.UInt(" +
             Operand(0) + "))!.loadUnaligned(as: " + uintType(Bits) + ".self))";
    default:
      throw Unsupported("unsupported Swift expression (including calls, "
                        "unresolved values, or fields)");
    }
  }

  void line(const std::string &Text) {
    Statements += std::string(Indent * 4, ' ') + Text + "\n";
  }

  void block(const std::vector<HighStmt> &Body, unsigned Depth = 0) {
    if (Depth > 200)
      throw Unsupported("Swift statement nesting exceeds the projection limit");
    for (const HighStmt &ST : Body) {
      if (!FallsThrough)
        break;
      if (ST.Kind != StmtKind::Nop)
        ++Count;
      switch (ST.Kind) {
      case StmtKind::Nop:
        break;
      case StmtKind::Assign: {
        if (!ST.Dst || ST.Dst->Kind != ExprKind::Var)
          throw Unsupported("unsupported Swift assignment destination");
        if (ST.Dst->Var.Kind == MedVar::Param && ST.Dst->Var.RenameTag < 0 &&
            ST.Dst->Var.Id >= 0 &&
            static_cast<size_t>(ST.Dst->Var.Id) >= S.Parameters.size() &&
            S.SelfConvention != "direct-fields")
          throw Unsupported(
              "Swift native assignment overwrites its bound self context");
        const unsigned Bits = width(ST.Dst->Type);
        if ((Bits == 128 || (ST.Val && width(ST.Val->Type) == 128)) &&
            (!ST.Val || width(ST.Val->Type) != Bits))
          throw Unsupported("Swift assignment crosses a wide-container "
                            "boundary without an explicit lane operation");
        const std::string Value = Bits == 128
                                      ? expression(ST.Val)
                                      : narrowed(expression(ST.Val), Bits);
        line(variable(ST.Dst->Var, Bits) + " = " + Value);
        Defined.insert(varKey(ST.Dst->Var));
        break;
      }
      case StmtKind::Call:
        line(call(ST.CallExpr, Depth + 1, true));
        break;
      case StmtKind::Return:
        if (Initializer) {
          if (S.ReturnsContextValue) {
            if (LoopDepth || SwitchDepth)
              throw Unsupported("Swift value initialization inside a loop or "
                                "switch lacks path proof");
            const auto &Field = S.ContextFields.front();
            if (Initialized.count(Field.Name))
              throw Unsupported("Swift scalar initializer has multiple stores "
                                "on one return path");
            line("self." + fieldName(Field) + " = " +
                 bitsToSource(expression(ST.RetVal), Field.Type));
            Initialized.insert(Field.Name);
          } else if (!isSelf(ST.RetVal)) {
            throw Unsupported(
                "Swift initializer does not return its bound self value");
          }
          requireInitialized();
          line("return");
        } else if (S.ReturnType.TheKind == SwiftSourceType::Kind::Void) {
          if (ST.RetVal)
            throw Unsupported(
                "void Swift method retains a machine return value");
          line("return");
        } else {
          if (ST.RetVal && width(ST.RetVal->Type) > 64)
            throw Unsupported(
                "Swift scalar return retains an unprojected wide container");
          line("return " + bitsToSource(expression(ST.RetVal), S.ReturnType));
        }
        FallsThrough = false;
        break;
      case StmtKind::Store: {
        if (ST.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
            ST.MemoryOrdering != NdMemoryOrdering::None)
          throw Unsupported(
              "unsupported ordered or non-default Swift memory write");
        const std::string Value = expression(ST.StoreVal);
        if (const auto *Field =
                field(ST.StoreAddr, width(ST.StoreVal->Type) / 8)) {
          if (S.ContextKind == "struct" && !S.IsMutating)
            throw Unsupported(
                "Swift nonmutating value method writes a stored property");
          if (!Field->IsMutable &&
              (!Initializer || Initialized.count(Field->Name)))
            throw Unsupported("Swift body writes an immutable stored property "
                              "after initialization");
          if (Initializer && (LoopDepth || SwitchDepth))
            throw Unsupported("Swift property initialization inside a loop or "
                              "switch lacks path proof");
          line("self." + fieldName(*Field) + " = " +
               bitsToSource(Value, Field->Type));
          Initialized.insert(Field->Name);
          break;
        }
        if (containsSelf(ST.StoreAddr))
          throw Unsupported("Swift self memory write does not match a proven "
                            "stored property");
        const std::string Address = expression(ST.StoreAddr);
        if (width(ST.StoreVal->Type) == 128) {
          if (width(ST.StoreAddr->Type) != 64)
            throw Unsupported("Swift wide store has no scalar address");
          line("(" + Value +
               ").store(Swift.UnsafeMutableRawPointer(bitPattern: Swift.UInt(" +
               Address + "))!)");
          break;
        }
        const std::string Type = uintType(width(ST.StoreVal->Type));
        line("Swift.UnsafeMutableRawPointer(bitPattern: Swift.UInt(" + Address +
             "))!.storeBytes(of: " + Type + "(truncatingIfNeeded: " + Value +
             "), as: " + Type + ".self)");
        break;
      }
      case StmtKind::If:
      case StmtKind::IfElse: {
        line("if " + condition(ST.Cond) + " {");
        const auto Before = flowState();
        std::optional<FlowState> Normal;
        ++Indent;
        block(ST.Body, Depth + 1);
        --Indent;
        if (FallsThrough)
          mergeFlow(Normal, flowState());
        restoreFlow(Before);
        if (!ST.ElseBody.empty()) {
          line("} else {");
          ++Indent;
          block(ST.ElseBody, Depth + 1);
          --Indent;
        }
        if (FallsThrough)
          mergeFlow(Normal, flowState());
        finishFlow(Normal, Before);
        line("}");
        break;
      }
      case StmtKind::While: {
        const auto Known = constantCondition(ST.Cond);
        line("while " + condition(ST.Cond) + " {");
        const auto Before = flowState();
        Controls.push_back({true, {}, {}});
        ++LoopDepth;
        ++Indent;
        block(ST.Body, Depth + 1);
        --Indent;
        --LoopDepth;
        auto Exits = std::move(Controls.back().BreakExits);
        Controls.pop_back();
        // Every normal exit of while(true) is a break owned by this loop.
        // A general while also has the zero-iteration condition-false edge.
        if (!Known || !*Known)
          mergeFlow(Exits, Before);
        finishFlow(Exits, Before);
        line("}");
        break;
      }
      case StmtKind::DoWhile: {
        line("repeat {");
        const auto Before = flowState();
        Controls.push_back({true, {}, {}});
        ++LoopDepth;
        ++Indent;
        block(ST.Body, Depth + 1);
        --Indent;
        --LoopDepth;
        auto Scope = std::move(Controls.back());
        Controls.pop_back();
        auto ConditionEdges = std::move(Scope.ContinueEdges);
        if (FallsThrough)
          mergeFlow(ConditionEdges, flowState());
        // continue in repeat/while reaches the condition. It cannot borrow
        // definitions from statements that follow that continue in the body.
        restoreFlow(ConditionEdges ? *ConditionEdges : Before);
        line("} while " + condition(ST.Cond));
        const auto Known = constantCondition(ST.Cond);
        if (ConditionEdges && (!Known || !*Known))
          mergeFlow(Scope.BreakExits, *ConditionEdges);
        finishFlow(Scope.BreakExits, Before);
        break;
      }
      case StmtKind::Block:
        block(ST.Body, Depth + 1);
        break;
      case StmtKind::Break:
        if (Controls.empty())
          throw Unsupported("break outside a loop or switch");
        mergeFlow(Controls.back().BreakExits, flowState());
        line("break");
        FallsThrough = false;
        break;
      case StmtKind::Continue: {
        auto Loop = std::find_if(Controls.rbegin(), Controls.rend(),
                                 [](const auto &Scope) { return Scope.Loop; });
        if (Loop == Controls.rend())
          throw Unsupported("continue outside a loop");
        mergeFlow(Loop->ContinueEdges, flowState());
        line("continue");
        FallsThrough = false;
        break;
      }
      case StmtKind::Switch: {
        line("switch " + expression(ST.SwitchExpr) + " {");
        const auto Before = flowState();
        Controls.push_back({false, {}, {}});
        ++SwitchDepth;
        std::set<uint64_t> Cases;
        auto Arm = [&](const std::vector<HighStmt> &Body) {
          restoreFlow(Before);
          ++Indent;
          block(Body, Depth + 1);
          if (FallsThrough) {
            mergeFlow(Controls.back().BreakExits, flowState());
            line("break");
          }
          --Indent;
        };
        for (const SwitchCase &Case : ST.Cases) {
          if (!Cases.insert(Case.Value).second)
            throw Unsupported("duplicate Swift switch cases");
          line("case " + std::to_string(Case.Value) + ":");
          Arm(Case.Body);
        }
        line("default:");
        Arm(ST.DefaultBody);
        auto Exits = std::move(Controls.back().BreakExits);
        Controls.pop_back();
        --SwitchDepth;
        finishFlow(Exits, Before);
        line("}");
        break;
      }
      default:
        throw Unsupported("unsupported Swift control flow or call statement");
      }
    }
  }

public:
  Writer(const HighFunc &Function, const SwiftSourceSignature &Signature,
         const std::vector<SwiftSourceSignature> &Callees)
      : F(Function), S(Signature), Callees(Callees) {}
  std::vector<va_t> dependencies() const {
    return {Dependencies.begin(), Dependencies.end()};
  }
  const std::string &memberSource() const { return Member; }

  std::string emit() {
    if (!S.UnsupportedReason.empty())
      throw Unsupported(S.UnsupportedReason);
    if (F.Entry != S.Entry || S.MangledSymbol.empty() || F.Body.empty())
      throw Unsupported("Swift signature identity or native body is missing");
    if (!identifier(S.Module) ||
        (S.ContextKind != "global" && S.ContextKind != "class" &&
         S.ContextKind != "struct"))
      throw Unsupported("unsupported Swift declaration context");
    if (S.ContextKind != "global")
      (void)name(S.ContextName);
    for (unsigned Suffix = 0;; ++Suffix) {
      WideType = "nd_word128_" + std::to_string(Suffix);
      if (WideType != S.Name && WideType != S.ContextName &&
          std::none_of(Callees.begin(), Callees.end(), [&](const auto &Callee) {
            return WideType == Callee.Name || WideType == Callee.ContextName;
          }))
        break;
    }
    Initializer = S.DeclarationKind == "initializer";
    const bool Getter = S.DeclarationKind == "getter";
    const bool Setter = S.DeclarationKind == "setter";
    const bool Accessor = Getter || Setter;
    const bool StructInitializer = Initializer && S.ContextKind == "struct";
    const bool StructInstance =
        S.ContextKind == "struct" && !S.IsStatic && !StructInitializer;
    if (StructInstance &&
        (!S.ContextLayoutKnown || !S.IsMutatingKnown ||
         S.ContextFields.empty() ||
         (S.SelfConvention != "direct-fields" &&
          S.SelfConvention != "indirect-mutating") ||
         (S.IsMutating != (S.SelfConvention == "indirect-mutating"))))
      throw Unsupported(
          "value-type self layout and calling convention are not established");
    if (S.DeclarationKind != "function" && !Initializer && !Accessor)
      throw Unsupported("unsupported Swift executable declaration kind");
    if (Accessor &&
        (S.IsStatic || S.ContextKind == "global" ||
         (Getter && (!S.Parameters.empty() ||
                     S.ReturnType.TheKind == SwiftSourceType::Kind::Void)) ||
         (Setter && (S.Parameters.size() != 1 ||
                     S.ReturnType.TheKind != SwiftSourceType::Kind::Void))))
      throw Unsupported(
          "Swift accessor requires a complete instance property signature");
    if (Initializer &&
        (S.IsStatic || !S.ContextLayoutKnown || S.Name != "init" ||
         (S.ContextKind != "class" && !StructInitializer) ||
         (!StructInitializer &&
          S.ReturnType.TheKind != SwiftSourceType::Kind::Pointer)))
      throw Unsupported("Swift initializer requires a proven class storage "
                        "layout and self return binding");
    if (StructInitializer &&
        (!S.ReturnsContextValue || S.ContextFields.size() != 1 ||
         S.ContextFields[0].Offset != 0 || !F.ReturnType ||
         F.ReturnType->Size != 8 ||
         !typeMatches(F.ReturnType, S.ContextFields[0].Type)))
      throw Unsupported("Swift value initializer requires a proven "
                        "single-field full-width scalar return");
    if (S.ReturnsContextValue && !StructInitializer)
      throw Unsupported("Swift complete-value return binding is not an "
                        "initializing struct declaration");
    std::set<std::string> FieldNames, StorageNames;
    std::set<uint64_t> FieldBytes;
    for (const auto &Field : S.ContextFields) {
      (void)name(Field.Name);
      (void)fieldName(Field);
      (void)swiftType(Field.Type);
      const unsigned Bytes =
          Field.Type.TheKind == SwiftSourceType::Kind::Pointer ? 8
          : Field.Type.TheKind == SwiftSourceType::Kind::Boolean
              ? 1
              : Field.Type.Bits / 8;
      if (!Bytes || Bytes > 8 || Field.Offset > 1024 * 1024 ||
          !FieldNames.insert(Field.Name).second ||
          !StorageNames
               .insert(Field.BackingName.empty() ? Field.Name
                                                 : Field.BackingName)
               .second)
        throw Unsupported(
            "Swift context contains an invalid or duplicate stored property");
      if (Accessor && Field.Name == S.Name && Field.BackingName.empty())
        throw Unsupported(
            "Swift accessor storage has no context-wide backing name");
      for (unsigned I = 0; I < Bytes; ++I)
        if (!FieldBytes.insert(Field.Offset + I).second)
          throw Unsupported("Swift context stored properties overlap");
    }
    const size_t Hidden =
        S.ContextKind == "class" ? 1
        : StructInstance
            ? (S.SelfConvention == "direct-fields" ? S.ContextFields.size() : 1)
            : 0;
    if (F.Params.size() != S.Parameters.size() + Hidden ||
        S.Labels.size() != S.Parameters.size() ||
        !typeMatches(F.ReturnType, S.ReturnType))
      throw Unsupported(
          "native Swift signature disagrees with the source declaration");
    std::string Signature;
    if (Accessor) {
      if (Getter && StructInstance && S.IsMutating)
        Signature += "mutating ";
      if (Setter && StructInstance && !S.IsMutating)
        Signature += "nonmutating ";
      Signature += Getter ? "get {\n" : "set(arg0) {\n";
    } else {
      Signature = "public ";
      if (S.IsStatic)
        Signature += "static ";
      if (StructInstance && S.IsMutating)
        Signature += "mutating ";
      Signature += Initializer ? "init(" : "func " + name(S.Name) + "(";
    }
    std::string Aliases;
    for (size_t I = 0; I < S.Parameters.size(); ++I) {
      const auto &Parameter = S.Parameters[I];
      if (Parameter.Name != "arg" + std::to_string(I) ||
          F.Params[I].Name != Parameter.Name ||
          !typeMatches(F.Params[I].Type, Parameter.Type) ||
          Parameter.Type.TheKind == SwiftSourceType::Kind::Void)
        throw Unsupported(
            "native Swift parameter binding disagrees with its signature");
      if (!Accessor) {
        if (I)
          Signature += ", ";
        Signature += (S.Labels[I] == "_" ? "_" : name(S.Labels[I])) + " " +
                     Parameter.Name + ": " + swiftType(Parameter.Type);
      }
      Aliases += "    var nd_arg" + std::to_string(I) + ": Swift.UInt64 = " +
                 sourceToBits(Parameter.Name, Parameter.Type) + "\n";
    }
    if (StructInstance && S.SelfConvention == "direct-fields") {
      for (size_t I = 0; I < Hidden; ++I) {
        const auto &Parameter = F.Params[S.Parameters.size() + I];
        if (Parameter.Name != "swift_self_" + std::to_string(I) ||
            !typeMatches(Parameter.Type, S.ContextFields[I].Type))
          throw Unsupported("Swift direct value self parameter does not match "
                            "its stored field");
        Aliases += "    var nd_arg" + std::to_string(S.Parameters.size() + I) +
                   ": Swift.UInt64 = " +
                   sourceToBits("self." + fieldName(S.ContextFields[I]),
                                S.ContextFields[I].Type) +
                   "\n";
      }
    } else if (Hidden) {
      const auto &Self = F.Params.back();
      if (Self.Name != "swift_self" || !Self.Type ||
          Self.Type->Kind != NdTypeKind::Ptr || Self.Type->Size != 8)
        throw Unsupported("Swift context register has no typed self binding");
      if (S.IsStatic) {
        const std::string Value =
            "Swift.UInt64(Swift.unsafeBitCast(Self.self, to: Swift.UInt.self))";
        Aliases += "    var nd_arg" + std::to_string(S.Parameters.size()) +
                   ": Swift.UInt64 = " + Value + "\n";
      }
    }
    if (!Accessor)
      Signature +=
          Initializer ? ") {\n" : ") -> " + swiftType(S.ReturnType) + " {\n";
    block(F.Body);
    if (Initializer && FallsThrough)
      requireInitialized();
    if (!Initializer && FallsThrough &&
        S.ReturnType.TheKind != SwiftSourceType::Kind::Void)
      throw Unsupported(
          "non-void Swift method has an uncovered fallthrough exit");
    if (!Count)
      throw Unsupported("native Swift body has no executable statements");
    std::string Locals;
    for (const auto &[Key, Variable] : Variables)
      Locals += "    var " + Variable + ": " +
                (VariableWidths.at(Key) == 128 ? WideType : "Swift.UInt64") +
                "\n";
    Member = Signature + wideHelper() + Aliases + Locals + Statements + "}\n";
    std::string Result = Member;
    if (S.ContextKind != "global" && !Initializer && !Accessor)
      Result = "extension " + name(S.ContextName) + " {\n" + Result + "}\n";
    return Result;
  }
};
} // namespace

SwiftEmissionResult
HighSwiftEmitter::emit(const HighFunc &Function,
                       const SwiftSourceSignature &Signature,
                       const std::vector<SwiftSourceSignature> &Callees) const {
  SwiftEmissionResult Result;
  try {
    Writer SourceWriter(Function, Signature, Callees);
    Result.Source = SourceWriter.emit();
    Result.MemberSource = SourceWriter.memberSource();
    Result.Dependencies = SourceWriter.dependencies();
    Result.Recovered = true;
    Result.Limitations = {"Swift bodies are machine-code projections; original "
                          "source and semantic equivalence are not guaranteed.",
                          "Instance field accesses require proven storage "
                          "metadata; compatible context declarations and "
                          "recovered initializers are assembled separately."};
  } catch (const Unsupported &Error) {
    Result.Reason = Error.what();
  }
  return Result;
}
} // namespace neverd
