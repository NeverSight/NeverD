//===- MIRPassRunnerRewrite.cpp - RewriteResult MIR pass runner -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Machine IR pass runner for RewriteResult.
///
//===----------------------------------------------------------------------===//

#include "neverd/object/SectionNames.h"
#include "neverd/pass/mir/MIRPass.h"
#include "neverd/support/BinaryEncoding.h"

#define DEBUG_TYPE "neverd-mir-pass"
#include "llvm/Support/Debug.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>

namespace neverd {

bool MIRPassRunner::runOnRewriteResult(llvm::mc_rewrite::RewriteResult &RR,
                                       Arch TheArch, InstructionMode Mode) {
  if (Passes.empty())
    return true;

  llvm::mc_rewrite::RewriteSection *Text = nullptr;
  for (auto &S : RR.Sections) {
    if (section_names::isTextSectionName(S.Name)) {
      Text = &S;
      break;
    }
  }
  if (!Text || Text->Bytes.empty()) {
    LLVM_DEBUG(llvm::dbgs() << "MIRPassRunner: no .text in RewriteResult\n");
    return false;
  }

  // Build sorted function list from SymbolAddrs (functions are symbols
  // whose VA falls within the .text range).
  struct FuncInfo {
    std::string Name;
    uint64_t VA;
    uint64_t Offset; // offset within Text->Bytes
    uint64_t Size;
  };
  std::vector<FuncInfo> Funcs;
  if (Text->Bytes.size() > InvalidVA - Text->VA)
    return false;
  uint64_t TextEnd = Text->VA + Text->Bytes.size();
  for (auto &[Name, VA] : RR.SymbolAddrs) {
    if (VA >= Text->VA && VA < TextEnd &&
        !llvm::StringRef(Name).starts_with("."))
      Funcs.push_back({Name, VA, VA - Text->VA, 0});
  }
  if (Funcs.empty())
    return false;

  std::sort(Funcs.begin(), Funcs.end(),
            [](const FuncInfo &A, const FuncInfo &B) { return A.VA < B.VA; });
  for (size_t I = 0; I < Funcs.size(); ++I) {
    if (I + 1 < Funcs.size())
      Funcs[I].Size = Funcs[I + 1].VA - Funcs[I].VA;
    else
      Funcs[I].Size = TextEnd - Funcs[I].VA;
  }

  bool AnyModified = false;
  for (auto &F : Funcs) {
    if (F.Size == 0 ||
        !rangeInBounds(F.Offset, F.Size, Text->Bytes.size()))
      continue;

    std::vector<uint8_t> FuncCode(Text->Bytes.begin() + F.Offset,
                                  Text->Bytes.begin() + F.Offset + F.Size);
    MIRPassContext Ctx;
    Ctx.Code = &FuncCode;
    Ctx.BaseVA = F.VA;
    Ctx.TheArch = TheArch;
    Ctx.Mode = Mode;
    Ctx.FuncName = F.Name;

    bool Modified = false;
    for (auto *Pass : Passes) {
      if (Pass->run(Ctx)) {
        LLVM_DEBUG(llvm::dbgs() << "MIRPassRunner: pass '" << Pass->name()
                                << "' modified '" << F.Name << "'\n");
        Modified = true;
      }
    }

    if (Modified) {
      if (FuncCode.size() == F.Size) {
        std::copy(FuncCode.begin(), FuncCode.end(),
                  Text->Bytes.begin() + F.Offset);
        AnyModified = true;
      } else {
        // A size change cannot be applied in the in-place rewrite path: the
        // RewriteResult already has every cross-function fixup (call targets,
        // PC-relative references) resolved against the original layout, so
        // growing or shrinking a function would silently break every reference
        // past it. Keep the function's original, correct bytes and report the
        // dropped change. Use the section-rebuild path (runOnCodegen) when a
        // pass needs to change function sizes. AnyModified is intentionally
        // left untouched so the return value reflects only changes that were
        // actually written back.
        llvm::WithColor::warning()
            << "MIRPassRunner: size change in '" << F.Name
            << "' skipped in the in-place rewrite path (would break resolved "
               "cross-function references)\n";
      }
    }
  }
  return AnyModified;
}

} // namespace neverd
