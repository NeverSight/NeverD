//===- MIRPassRunner.cpp - Machine IR pass runner ------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Machine IR pass runner infrastructure.
///
//===----------------------------------------------------------------------===//

#include "neverd/loader/BinaryImage.h"
#include "neverd/pass/mir/MIRPass.h"

#define DEBUG_TYPE "neverd-mir-pass"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <limits>

namespace neverd {

void MIRPassRunner::addPass(MIRPass *P) { Passes.push_back(P); }

static void computeFunctionSizes(std::vector<CodegenResult::FuncEntry> &Funcs,
                                 uint64_t TextSectionAddr,
                                 uint64_t TextSectionSize) {
  std::sort(Funcs.begin(), Funcs.end(),
            [](const auto &A, const auto &B) { return A.Offset < B.Offset; });

  uint64_t TextEnd = TextSectionAddr + TextSectionSize;
  for (size_t I = 0; I < Funcs.size(); ++I) {
    if (Funcs[I].Offset < TextSectionAddr || Funcs[I].Offset >= TextEnd) {
      Funcs[I].Size = 0;
      continue;
    }
    if (Funcs[I].Size != 0)
      continue;
    if (I + 1 < Funcs.size() && Funcs[I + 1].Offset > Funcs[I].Offset &&
        Funcs[I + 1].Offset <= TextEnd)
      Funcs[I].Size = Funcs[I + 1].Offset - Funcs[I].Offset;
    else
      Funcs[I].Size = TextEnd - Funcs[I].Offset;
  }
}

bool MIRPassRunner::runOnCodegen(CodegenResult &CG, Arch TheArch,
                                 InstructionMode Mode) {
  if (Passes.empty())
    return true;
  if (CG.Functions.empty()) {
    llvm::WithColor::warning()
        << "MIRPassRunner: no functions in codegen result\n";
    return false;
  }

  auto Buf = llvm::MemoryBuffer::getMemBuffer(
      llvm::StringRef(reinterpret_cast<const char *>(CG.ObjectData.data()),
                      CG.ObjectData.size()),
      "", false);
  auto ObjOrErr =
      llvm::object::ObjectFile::createObjectFile(Buf->getMemBufferRef());
  if (!ObjOrErr) {
    llvm::consumeError(ObjOrErr.takeError());
    llvm::WithColor::error() << "MIRPassRunner: failed to parse object file\n";
    return false;
  }

  uint64_t TextFileOffset = 0;
  uint64_t TextSectionAddr = 0;
  uint64_t TextSectionSize = 0;

  for (auto &Sec : (**ObjOrErr).sections()) {
    auto Name = Sec.getName();
    if (Name && section_names::isTextSectionName(*Name)) {
      auto Content = Sec.getContents();
      if (!Content)
        continue;
      TextSectionAddr = Sec.getAddress();
      TextSectionSize = Content->size();

      auto DataStart = reinterpret_cast<const uint8_t *>(Content->data());
      auto ObjStart = CG.ObjectData.data();
      TextFileOffset = static_cast<uint64_t>(DataStart - ObjStart);
      break;
    }
  }

  if (TextSectionSize == 0) {
    llvm::WithColor::warning() << "MIRPassRunner: no text section found\n";
    return false;
  }
  if (TextSectionSize > InvalidVA - TextSectionAddr ||
      !rangeInBounds(TextFileOffset, TextSectionSize, CG.ObjectData.size())) {
    llvm::WithColor::warning()
        << "MIRPassRunner: invalid text section range\n";
    return false;
  }

  computeFunctionSizes(CG.Functions, TextSectionAddr, TextSectionSize);
  for (const auto &Func : CG.Functions) {
    if (Func.Size == 0)
      continue;
    if (Func.Offset < TextSectionAddr ||
        !rangeInBounds(Func.Offset - TextSectionAddr, Func.Size,
                       TextSectionSize)) {
      llvm::WithColor::warning()
          << "MIRPassRunner: function '" << Func.Name
          << "' lies outside the text section\n";
      return false;
    }
  }

  struct FuncPatch {
    size_t FuncIdx;
    uint64_t OrigFileStart;
    uint64_t OrigSize;
    std::vector<uint8_t> NewCode;
  };
  std::vector<FuncPatch> Patches;

  for (size_t FI = 0; FI < CG.Functions.size(); ++FI) {
    auto &Func = CG.Functions[FI];
    if (Func.Size == 0)
      continue;

    uint64_t FuncFileStart = TextFileOffset + (Func.Offset - TextSectionAddr);
    if (FuncFileStart + Func.Size > CG.ObjectData.size()) {
      llvm::WithColor::warning() << "MIRPassRunner: function '" << Func.Name
                                 << "' out of bounds (off=" << FuncFileStart
                                 << ", sz=" << Func.Size << ")" << "\n";
      continue;
    }

    std::vector<uint8_t> FuncCode(CG.ObjectData.begin() + FuncFileStart,
                                  CG.ObjectData.begin() + FuncFileStart +
                                      Func.Size);

    MIRPassContext Ctx;
    Ctx.Code = &FuncCode;
    Ctx.BaseVA = Func.Offset;
    Ctx.TheArch = TheArch;
    Ctx.Mode = Mode;
    Ctx.FuncName = Func.Name;

    bool Modified = false;
    for (auto *Pass : Passes) {
      if (Pass->run(Ctx)) {
        LLVM_DEBUG(llvm::dbgs()
                   << "MIRPassRunner: pass '" << Pass->name()
                   << "' modified function '" << Func.Name << "'" << "\n");
        Modified = true;
      }
    }

    if (Modified) {
      Patches.push_back({FI, FuncFileStart, Func.Size, std::move(FuncCode)});
    }
  }

  if (Patches.empty())
    return false;

  bool HasSizeChanges =
      std::any_of(Patches.begin(), Patches.end(), [](const FuncPatch &P) {
        return P.NewCode.size() != P.OrigSize;
      });

  if (!HasSizeChanges) {
    for (auto &P : Patches) {
      std::copy(P.NewCode.begin(), P.NewCode.end(),
                CG.ObjectData.begin() + P.OrigFileStart);
    }
    LLVM_DEBUG(llvm::dbgs() << "MIRPassRunner: " << Patches.size()
                            << " functions patched in-place" << "\n");
    return true;
  }

  std::vector<uint8_t> NewText;
  NewText.reserve(static_cast<size_t>(TextSectionSize));

  std::sort(Patches.begin(), Patches.end(), [](const auto &A, const auto &B) {
    return A.OrigFileStart < B.OrigFileStart;
  });

  size_t PatchIdx = 0;
  size_t OriginalCursor = static_cast<size_t>(TextFileOffset);

  for (size_t FI = 0; FI < CG.Functions.size(); ++FI) {
    auto &Func = CG.Functions[FI];
    if (Func.Size == 0)
      continue;

    size_t FuncFileStart = static_cast<size_t>(
        TextFileOffset + (Func.Offset - TextSectionAddr));
    if (FuncFileStart < OriginalCursor)
      return false;
    NewText.insert(NewText.end(), CG.ObjectData.begin() + OriginalCursor,
                   CG.ObjectData.begin() + FuncFileStart);
    size_t NewFuncStart = NewText.size();

    if (PatchIdx < Patches.size() && Patches[PatchIdx].FuncIdx == FI) {
      auto &P = Patches[PatchIdx];
      NewText.insert(NewText.end(), P.NewCode.begin(), P.NewCode.end());
      Func.Size = P.NewCode.size();
      Func.Offset = TextSectionAddr + NewFuncStart;
      OriginalCursor = FuncFileStart + static_cast<size_t>(P.OrigSize);
      ++PatchIdx;
    } else {
      auto Start = CG.ObjectData.begin() + FuncFileStart;
      Func.Offset = TextSectionAddr + NewFuncStart;
      NewText.insert(NewText.end(), Start, Start + Func.Size);
      OriginalCursor = FuncFileStart + static_cast<size_t>(Func.Size);
    }
  }

  auto TextEnd = TextFileOffset + TextSectionSize;
  NewText.insert(NewText.end(), CG.ObjectData.begin() + OriginalCursor,
                 CG.ObjectData.begin() + static_cast<size_t>(TextEnd));
  std::vector<uint8_t> NewObj;
  size_t PrefixSize = static_cast<size_t>(TextFileOffset);
  size_t SuffixSize = CG.ObjectData.size() - static_cast<size_t>(TextEnd);
  if (NewText.size() >
      std::numeric_limits<size_t>::max() - PrefixSize - SuffixSize)
    return false;
  NewObj.reserve(PrefixSize + NewText.size() + SuffixSize);
  NewObj.insert(NewObj.end(), CG.ObjectData.begin(),
                CG.ObjectData.begin() + TextFileOffset);
  NewObj.insert(NewObj.end(), NewText.begin(), NewText.end());
  NewObj.insert(NewObj.end(), CG.ObjectData.begin() + TextEnd,
                CG.ObjectData.end());
  CG.ObjectData = std::move(NewObj);

  LLVM_DEBUG(
      llvm::dbgs() << "MIRPassRunner: " << Patches.size()
                   << " functions patched ("
                   << std::count_if(Patches.begin(), Patches.end(),
                                    [](const FuncPatch &P) {
                                      return P.NewCode.size() != P.OrigSize;
                                    })
                   << " with size changes, text section " << TextSectionSize
                   << " -> " << NewText.size() << " bytes)\n");

  return true;
}

bool MIRPassRunner::runOnRewriteResult(llvm::mc_rewrite::RewriteResult &RR,
                                       Arch TheArch, InstructionMode Mode) {
  if (Passes.empty())
    return true;

  llvm::mc_rewrite::RewriteSection *Text = nullptr;
  for (auto &S : RR.Sections) {
    llvm::StringRef N(S.Name);
    if (N.contains("text") || N.contains("TEXT")) {
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
