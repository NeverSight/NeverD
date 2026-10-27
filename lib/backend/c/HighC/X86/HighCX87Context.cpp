//===- HighCX87Context.cpp - Readable owned x87 C operations ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Emit named operations while keeping complete state transport in one body.
//===----------------------------------------------------------------------===//
#include "../HighCWriter.h"

#include "neverd/backend/c/render/X86/X87ContextRender.h"
#include "neverd/ir/high/X86/X87ContextShape.h"

#include "llvm/ADT/StringExtras.h"

#include <functional>
namespace neverd {
namespace {
using Key = std::tuple<Intrinsic, uint32_t, NdMemoryAddressSpace>;
Key key(const HighExpr &E) {
  uint32_t Selector = 0;
  if (E.IntrinsicId == Intrinsic::X87ContextStep ||
      E.IntrinsicId == Intrinsic::X87ContextMMXRead ||
      E.IntrinsicId == Intrinsic::X87ContextMMXWrite)
    Selector = E.Operands[1]->ConstVal;
  return {E.IntrinsicId, Selector, E.MemoryAddressSpace};
}
std::string helperStem(const Key &K) {
  const auto [Id, Selector, Space] = K;
  if (Id != Intrinsic::X87ContextStep)
    return std::string(intrinsicCName(Id)) +
           (Id == Intrinsic::X87ContextMMXRead ||
                    Id == Intrinsic::X87ContextMMXWrite
                ? "_" + std::to_string(Selector)
                : "");
  const unsigned Escape = Selector & 0xff, ModRM = (Selector >> 8) & 0xff;
  std::string Name = "operation";
  if (Selector & X87ContextThreeDNow) {
    Name =
        llvm::StringRef(
            x86ThreeDNowInstruction(x87ContextThreeDNowOpcode(Selector))->Name)
            .lower();
    Name += "_mm" + std::to_string((ModRM >> 3) & 7);
    Name += (Selector & X87ContextMemory) ? "_m64"
                                          : "_mm" + std::to_string(ModRM & 7);
  } else if (Selector & X87ContextMemory) {
    const char *D9[] = {"fld",    "invalid", "fst",     "fstp",
                        "fldenv", "fldcw",   "fnstenv", "fnstcw"};
    const char *DB[] = {"fild",    "fisttp", "fist",    "fistp",
                        "invalid", "fld",    "invalid", "fstp"};
    const char *DD[] = {"fld",    "fisttp",  "fst",    "fstp",
                        "frstor", "invalid", "fnsave", "fnstsw"};
    const char *DF[] = {"fild", "fisttp", "fist",  "fistp",
                        "fbld", "fild",   "fbstp", "fistp"};
    const char *Arithmetic[] = {"fadd", "fmul",  "fcom", "fcomp",
                                "fsub", "fsubr", "fdiv", "fdivr"};
    const unsigned Group = ModRM >> 3;
    Name = Escape == 0xd9   ? D9[Group]
           : Escape == 0xdb ? DB[Group]
           : Escape == 0xdd ? DD[Group]
           : Escape == 0xdf ? DF[Group]
                            : Arithmetic[Group];
    if (Escape == 0xda || Escape == 0xde)
      Name.insert(1, "i");
    Name +=
        "_m" + std::to_string(x87ContextInstruction(Selector)->MemoryBytes * 8);
  } else {
    if (Selector == 0xe3db)
      Name = "fninit";
    else if (Selector == 0xe2db)
      Name = "fnclex";
    else if (Selector == 0xe0df)
      Name = "fnstsw_ax";
    else if (Selector == 0xe0d9)
      Name = "fchs";
    else if (Selector == 0xe1d9)
      Name = "fabs";
    else if (Selector == 0xe5d9)
      Name = "fxam";
    else if (Selector == 0x9b)
      Name = "wait";
    else if (Selector == 0x770f)
      Name = "emms";
    else if (Escape == 0xd9 && ModRM >= 0xe8 && ModRM <= 0xee) {
      const char *Constants[] = {"fld1",   "fldl2t", "fldl2e", "fldpi",
                                 "fldlg2", "fldln2", "fldz"};
      Name = Constants[ModRM - 0xe8];
    } else if (Escape == 0xd9 && ModRM >= 0xf0) {
      const char *Unary[] = {"f2xm1",   "fyl2x",   "fptan",   "fpatan",
                             "fxtract", "fprem1",  "fdecstp", "fincstp",
                             "fprem",   "fyl2xp1", "fsqrt",   "fsincos",
                             "frndint", "fscale",  "fsin",    "fcos"};
      Name = Unary[ModRM - 0xf0];
    } else if (Escape == 0xd8 || Escape == 0xdc || Escape == 0xde) {
      const char *Arithmetic[] = {"fadd", "fmul",  "fcom", "fcomp",
                                  "fsub", "fsubr", "fdiv", "fdivr"};
      unsigned Group = (ModRM >> 3) & 7;
      if (Escape != 0xd8 && Group >= 4)
        Group ^= 1;
      Name = Arithmetic[Group];
      if (Escape == 0xde)
        Name = ModRM == 0xd9 ? "fcompp" : Name + "p";
      if (ModRM != 0xd9 || Escape != 0xde)
        Name += Escape == 0xd8 ? "_st0_st" + std::to_string(ModRM & 7)
                               : "_st" + std::to_string(ModRM & 7) + "_st0";
    } else if ((Escape == 0xda || Escape == 0xdb) && ModRM <= 0xdf) {
      const char *Moves[] = {"fcmovb",  "fcmove",  "fcmovbe",  "fcmovu",
                             "fcmovnb", "fcmovne", "fcmovnbe", "fcmovnu"};
      Name = Moves[((ModRM >> 3) & 3) + (Escape == 0xdb ? 4 : 0)];
      Name += "_st0_st" + std::to_string(ModRM & 7);
    } else if (Escape == 0xda && ModRM == 0xe9)
      Name = "fucompp";
    else if ((Escape == 0xdb || Escape == 0xdf) && ModRM >= 0xe8) {
      Name = ModRM < 0xf0 ? "fucomi" : "fcomi";
      if (Escape == 0xdf)
        Name += "p";
      Name += "_st0_st" + std::to_string(ModRM & 7);
    } else if (Escape == 0xdd || (Escape == 0xdf && ModRM <= 0xc7)) {
      const char *Registers[] = {"ffree", "invalid", "fst",
                                 "fstp",  "fucom",   "fucomp"};
      Name = Escape == 0xdf ? "ffreep" : Registers[(ModRM - 0xc0) >> 3];
      Name += "_st" + std::to_string(ModRM & 7);
    } else if (Escape == 0xd9 && ModRM <= 0xcf) {
      Name = ModRM < 0xc8 ? "fld" : "fxch";
      Name += "_st" + std::to_string(ModRM & 7);
    } else if (Escape == 0xd9 && ModRM == 0xd0)
      Name = "fnop";
    else if (Escape == 0xd9 && ModRM == 0xe4)
      Name = "ftst";
  }
  return "neverd_x87_" + Name + "_" + llvm::utohexstr(Selector, true) +
         (Space == NdMemoryAddressSpace::X86FS   ? "_fs"
          : Space == NdMemoryAddressSpace::X86GS ? "_gs"
                                                 : "");
}
std::string renderBody(Intrinsic Id, uint32_t Selector,
                       NdMemoryAddressSpace Space,
                       const std::vector<std::string> &Args,
                       CProjectionIdentifierAllocator &Names) {
  const std::string Name = Names.allocate("x87_context");
  const auto asmText = [&](X87ContextAsmPhase Phase, uint32_t Selector,
                           const std::vector<std::string> &Args) {
    return renderX87ContextAsm({Phase, Selector,
                                Phase == X87ContextAsmPhase::Step
                                    ? Space
                                    : NdMemoryAddressSpace::Default},
                               Args);
  };
  if (Id == Intrinsic::X87ContextBegin) {
    // Plain alloca retains the context for the whole function. An aligned
    // alloca builtin may instead end its lifetime with this expression block.
    const std::string Storage =
        Args.empty()
            ? "(((uintptr_t)__builtin_alloca(1039) + 15) & ~(uintptr_t)15)"
            : Args[0];
    return "({ unsigned char *" + Name + " = (unsigned char *)(" + Storage +
           "); "
           "__builtin_memset(" +
           Name + ", 0, 1024); " +
           asmText(X87ContextAsmPhase::Capture, 0, {Name}) +
           " (uint64_t)(uintptr_t)" + Name + "; })";
  }
  std::string Text = "({ unsigned char *" + Name +
                     " = (unsigned char *)(uintptr_t)(" + Args[0] + "); ";
  if (Id == Intrinsic::X87ContextRefresh)
    return Text + asmText(X87ContextAsmPhase::Capture, 0, {Name}) + " })";
  if (Id == Intrinsic::X87ContextMMXRead) {
    Text += "uint16_t sw; uint64_t value; __builtin_memcpy(&sw," + Name +
            "+2,2); __builtin_memcpy(&value," + Name + "+32+(((" + Args[1] +
            "+8-((sw>>11)&7))&7)*16),8); value; })";
    return Text;
  }
  if (Id == Intrinsic::X87ContextMMXEnter) {
    Text +=
        "uint16_t sw; unsigned char physical[128]; __builtin_memcpy(&sw," +
        Name +
        "+2,2); for (unsigned i=0;i<8;++i) __builtin_memcpy(physical+i*16," +
        Name + "+32+(((i+8-((sw>>11)&7))&7)*16),16); __builtin_memcpy(" + Name +
        "+32,physical,128); sw &= 0xc7ff; __builtin_memcpy(" + Name +
        "+2,&sw,2); " + Name + "[4] = 255; })";
    return Text;
  }
  if (Id == Intrinsic::X87ContextMMXWrite) {
    const std::string Slot = std::to_string(32 + Selector * 16);
    const std::string Exponent = std::to_string(40 + Selector * 16);
    Text += "uint64_t value = (uint64_t)(" + Args[2] +
            "); uint16_t exponent = 65535; __builtin_memcpy(" + Name + "+" +
            Slot + ",&value,8); __builtin_memcpy(" + Name + "+" + Exponent +
            ",&exponent,2); })";
    return Text;
  }
  Text += asmText(X87ContextAsmPhase::SaveHost, 0, {Name}) + " ";
  if (Id == Intrinsic::X87ContextExport) {
    Text += "__builtin_memcpy(" + Name + "+512," + Name +
            ",24); "
            "__builtin_memcpy(" +
            Name + "+544," + Name + "+32,128); ";
    return Text + asmText(X87ContextAsmPhase::RestoreHost, 0, {Name}) + " })";
  }
  const auto Spec = *x87ContextInstruction(Selector);
  const std::string Result = Names.allocate("x87_result");
  const std::string Flags = Names.allocate("x87_flags");
  const bool NormalizeIP = x87ContextUpdatesInstructionPointer(Selector);
  const std::string PreviousIP = Names.allocate("x87_previous_ip");
  const std::string PreviousOpcode = Names.allocate("x87_previous_opcode");
  Text += "__builtin_memcpy(" + Name + "+24," + Name +
          "+536,8); "
          "__builtin_memcpy(" +
          Name + "+160," + Name +
          "+672,352); "
          "uint64_t " +
          Result + " = 0, " + Flags + " = (uint64_t)(" + Args[3] + "); ";
  if (NormalizeIP) {
    Text += "uint64_t " + PreviousIP + "; __builtin_memcpy(&" + PreviousIP +
            "," + Name + "+8,8); ";
    Text += "uint16_t " + PreviousOpcode + "; __builtin_memcpy(&" +
            PreviousOpcode + "," + Name + "+6,2); ";
  }
  Text += asmText(X87ContextAsmPhase::Step, Selector,
                  {Name, "&" + Result, "(void *)(uintptr_t)(" + Args[2] + ")",
                   "&" + Flags});
  if (NormalizeIP) {
    const std::string CurrentIP = Names.allocate("x87_current_ip");
    const std::string Opcode = Names.allocate("x87_original_opcode");
    const unsigned Original =
        (Selector & X87ContextMemory)
            ? (Selector >> X87ContextOriginalModRMShift) & 0xff
            : (Selector >> 8) & 0xff;
    Text += "uint64_t " + CurrentIP + "; __builtin_memcpy(&" + CurrentIP + "," +
            Name + "+8,8); if (" + CurrentIP + " != " + PreviousIP + ") { " +
            CurrentIP + " = (uint64_t)(" + Args[4] + "); __builtin_memcpy(" +
            Name + "+8,&" + CurrentIP +
            ",8); } "
            "uint16_t current_opcode, current_status; "
            "__builtin_memcpy(&current_opcode," +
            Name + "+6,2); __builtin_memcpy(&current_status," + Name +
            "+2,2); if (current_opcode != " + PreviousOpcode +
            " || (current_status & 128)) { uint16_t " + Opcode + " = " +
            std::to_string(((Selector & 7) << 8) | Original) +
            "; "
            "__builtin_memcpy(" +
            Name + "+6,&" + Opcode + ",2); } ";
  }
  if (Spec.WritesAX)
    Text += " (uint16_t)" + Result + ";";
  else if (Spec.WritesFlags)
    Text += " (" + Result + " & 213) | ((" + Result + " >> 8 & 1) << 11);";
  return Text + " })";
}

} // namespace

std::string HighCWriter::renderX87ContextCall(const HighExpr &E) {
  if (!x87ContextHighShapeIsValid(E, Opts.TheArch))
    llvm::report_fatal_error("invalid HighC complete x87 context contract");
  const Key K = key(E);
  std::vector<std::string> Args;
  for (const auto &V : E.Operands)
    Args.push_back(exprStr(*V));
  if (!EmitFunctionWrapper) {
    const auto [Id, Selector, Space] = K;
    std::string Text = Id == Intrinsic::X87ContextBegin
                           ? "neverd_x87_begin_context"
                           : helperStem(K);
    Text += "(";
    bool First = true;
    for (unsigned I = 0; I < Args.size(); ++I) {
      if (I == 1 && (Id == Intrinsic::X87ContextStep ||
                     Id == Intrinsic::X87ContextMMXRead ||
                     Id == Intrinsic::X87ContextMMXWrite))
        continue;
      if (!First)
        Text += ", ";
      Text += Args[I];
      First = false;
    }
    return Text + ")";
  }
  if (E.IntrinsicId == Intrinsic::X87ContextBegin) {
    const auto Storage = memoryTemporary(
        "struct { _Alignas(16) unsigned char bytes[1024]; }", "x87_storage");
    return renderBody(E.IntrinsicId, 0, E.MemoryAddressSpace,
                      {Storage + ".bytes"}, MemoryIdentifiers);
  }
  const auto It = X87ContextHelperNames.find(K);
  if (It == X87ContextHelperNames.end())
    return renderBody(E.IntrinsicId, std::get<1>(K), E.MemoryAddressSpace, Args,
                      MemoryIdentifiers);
  std::string Text = It->second + "(";
  bool First = true;
  for (unsigned I = 0; I < Args.size(); ++I) {
    if (I == 1 && (E.IntrinsicId == Intrinsic::X87ContextStep ||
                   E.IntrinsicId == Intrinsic::X87ContextMMXRead ||
                   E.IntrinsicId == Intrinsic::X87ContextMMXWrite))
      continue;
    if (!First)
      Text += ", ";
    Text += "(uint64_t)(" + Args[I] + ")";
    First = false;
  }
  return Text + ")";
}

void HighCWriter::writeX87ContextHelpers(const std::vector<HighFunc> &Funcs) {
  std::set<const HighExpr *> Seen;
  std::function<void(const ExprPtr &)> Visit = [&](const ExprPtr &E) {
    if (!E || !Seen.insert(E.get()).second)
      return;
    if (E->Kind == ExprKind::Call && isX87ContextIntrinsic(E->IntrinsicId)) {
      if (!x87ContextHighShapeIsValid(*E, Opts.TheArch))
        llvm::report_fatal_error("invalid HighC complete x87 context contract");
      if (E->IntrinsicId != Intrinsic::X87ContextBegin)
        X87ContextHelperNames.try_emplace(key(*E), "");
    }
    for (const auto &Child : E->Operands)
      Visit(Child);
    Visit(E->IndirectTarget);
  };
  for (const auto &F : Funcs)
    walkStmts(F.Body, [&](const HighStmt &S) { forEachExpr(S, Visit); });
  for (auto &[K, Name] : X87ContextHelperNames) {
    Name = GlobalIdentifierAllocator.allocate(helperStem(K));
    const auto [Id, Selector, Space] = K;
    std::vector<std::string> Args{"handle"};
    std::string Params = "uint64_t handle";
    bool Returns = Id == Intrinsic::X87ContextMMXRead;
    if (Id == Intrinsic::X87ContextStep) {
      const auto Spec = *x87ContextInstruction(Selector);
      Returns = Spec.WritesAX || Spec.WritesFlags;
      Args = {"handle", std::to_string(Selector), "address", "flags",
              "source_ip"};
      Params += ", uint64_t address, uint64_t flags, uint64_t source_ip";
    } else if (Id == Intrinsic::X87ContextMMXRead)
      Args.push_back(std::to_string(Selector));
    else if (Id == Intrinsic::X87ContextMMXWrite) {
      Args = {"handle", std::to_string(Selector), "mmx_value"};
      Params += ", uint64_t mmx_value";
    }
    CProjectionIdentifierAllocator Locals;
    for (const auto &Arg : Args)
      (void)Locals.allocate(Arg);
    std::string Body = renderBody(Id, Selector, Space, Args, Locals);
    // Void GNU statement expressions may end with memcpy's pointer result.
    if (!Returns)
      Body.insert(Body.size() - 3, " (void)0; ");
    OS << "static inline __attribute__((always_inline)) "
       << (Returns ? "uint64_t " : "void ") << Name << "(" << Params
       << ") {\n    ";
    if (Returns)
      OS << "return ";
    OS << Body << ";\n}\n\n";
  }
}
} // namespace neverd
