//===- COFFRegistrationCxx.cpp - Checked PE32 C++ table closure -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "COFFRegistrationIRProof.h"
#include "COFFRegistrationTableProof.h"

#include "neverd/Limits.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/backend/llvm/WindowsEHNativeSource.h"
#include "neverd/backend/llvm/WindowsEHSemanticDigest.h"
#include "neverd/decode/Decoder.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/loader/ExceptionInfo.h"

#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Mangler.h"
#ifdef LLVM_NEVERD_X86_CXX_FUNCTION_RECEIPTS
#include "llvm/IR/WinEHFrame.h"
#endif
#include "llvm/Support/Endian.h"
#include "llvm/TargetParser/Triple.h"

#include <functional>
#include <set>

namespace neverd {
namespace {
llvm::Error rejectCxx(const llvm::Twine &Detail) {
  return llvm::createStringError(llvm::errc::invalid_argument,
                                 "coff registration C++ tables: " + Detail);
}
} // namespace

llvm::Expected<COFFRegistrationCxxTableReceipt>
getCheckedCOFFRegistrationCxxTableReceipt(const llvm::Function &Function,
                                          const ExceptionFunction &Source,
                                          const BinaryImage &Image,
                                          const CompiledImage &Compiled) {
#ifndef LLVM_NEVERD_X86_CXX_FUNCTION_RECEIPTS
  return rejectCxx("LLVM does not provide complete PE32 C++ table receipts");
#else
  using Kind = llvm::mc_rewrite::RewriteWinEHSemanticKind;
  using Encoding = llvm::mc_rewrite::RewriteWinEHSemanticEncoding;
  using SectionKind = llvm::mc_rewrite::RewriteSectionKind;
  const auto Classification =
      classifyWindowsEHNativeSource(Source, Arch::X86, BinaryFormat::COFF,
                                    WindowsEHNativeCapability::IRLowering);
  auto OriginalVA = rewrite_source::getOriginalVA(Function);
  if (!OriginalVA)
    return OriginalVA.takeError();
  if (*OriginalVA != Source.CodeRange.Begin ||
      Classification.Model != WindowsEHNativeSourceModel::X86RegistrationCxx ||
      !Classification.canLowerNativeIR() || Image.Arch != Arch::X86 ||
      Image.Format != BinaryFormat::COFF ||
      !coff_loader::getCheckedX86CxxMetadataRanges(Image, Source) ||
      !coff_loader::getCheckedX86CxxPersonalityABI(Image, Source) ||
      !Compiled.Success || !Compiled.Unresolved.empty() ||
      !Compiled.FunctionRangesValid || !Compiled.WinEHSemanticsValid ||
      Compiled.TargetArch != Arch::X86 ||
      Compiled.Format != BinaryFormat::COFF || Compiled.PointerWidth != 4 ||
      Compiled.ByteOrder != llvm::endianness::little ||
      llvm::Triple(Compiled.TargetTriple).getArch() != llvm::Triple::x86 ||
      !llvm::Triple(Compiled.TargetTriple).isWindowsMSVCEnvironment() ||
      !llvm::mc_rewrite::validateRewriteWinEHSemanticRecords(
          Compiled.WinEHSemanticRecords, Compiled.SourceFunctionOwners,
          Compiled.FunctionRanges, Compiled.FunctionOwnerAddrs))
    return rejectCxx("source or compiler contract is incomplete");
  const auto &Cxx = *Source.Cxx;
  const auto &Try = Cxx.TryBlocks[0];
  auto Root = coff_registration::ownerVA(Function, Compiled);
  if (!Root)
    return Root.takeError();
  std::vector<const CompiledWinEHSemanticRecord *> Catches(Try.Handlers.size());
  std::map<uint32_t, const CompiledWinEHSemanticRecord *> BySource, ByState;
  for (const auto &Row : Compiled.WinEHSemanticRecords) {
    if (Row.SourceFunction != Function.getName())
      continue;
    if (Row.OwnerVA != *Root || Row.Encoding != Encoding::X86CxxFH3 ||
        !Row.X86CxxLayout)
      return rejectCxx("row changed its generated function or encoding");
    if (Row.Token.Kind == Kind::CxxCatch) {
      const auto Token = windows_eh_semantics::getCxxCatchSemanticToken(
          Source, Arch::X86, Row.Token.Region, Row.Token.Clause);
      if (Row.Token.Region || Row.Token.Clause >= Catches.size() || !Token ||
          Row.Token != *Token || Catches[Row.Token.Clause])
        return rejectCxx("catch row has no unique source identity");
      Catches[Row.Token.Clause] = &Row;
    } else if (Row.Token.Kind == Kind::CxxCleanup) {
      const auto Token = windows_eh_semantics::getCxxCleanupSemanticToken(
          Source, Arch::X86, Row.Token.Region);
      if (!Token || Row.Token != *Token ||
          !BySource.emplace(Row.Token.Region, &Row).second ||
          !ByState.emplace(Row.GeneratedState, &Row).second)
        return rejectCxx("cleanup row has no unique source identity");
    } else {
      return rejectCxx("unrecognized row belongs to a C++ source function");
    }
  }
  if (llvm::is_contained(Catches, nullptr))
    return rejectCxx("generated FuncInfo lost a catch row");
  const auto &Layout = *Catches.front()->X86CxxLayout;
  for (const auto *Row : Catches) {
    const auto &Other = *Row->X86CxxLayout;
    if (Other.Tables != Layout.Tables ||
        Other.RegistrationFrame != Layout.RegistrationFrame ||
        Other.RegistrationHandlerVA != Layout.RegistrationHandlerVA ||
        Other.RegistrationHandlerSymbol != Layout.RegistrationHandlerSymbol)
      return rejectCxx(
          "catch rows disagree about their complete runtime tables");
  }
  for (const auto &Table : Layout.Tables)
    if (!coff_registration::sectionAt(Compiled, Table.BeginVA,
                                      Table.EndVA - Table.BeginVA,
                                      SectionKind::ReadOnlyData))
      return rejectCxx("language table has no unique generated byte extent");
  auto Word = [&](va_t VA) {
    return llvm::support::endian::read32le(Compiled.Bytes.data() + VA -
                                           Compiled.BaseVA);
  };
  const va_t Info = Layout.Tables[0].BeginVA;
  const va_t Unwind = Layout.Tables[1].BeginVA;
  const va_t TryMap = Layout.Tables[2].BeginVA;
  const va_t HandlerMap = Layout.Tables[3].BeginVA;
  const uint32_t MaxState = (Layout.Tables[1].EndVA - Unwind) / 8;
  if (!MaxState || MaxState > 130 || Word(Info) != 0x19930522 ||
      Word(Info + 4) != MaxState || Word(Info + 8) != Unwind ||
      Word(Info + 12) != 1 || Word(Info + 16) != TryMap || Word(Info + 20) ||
      Word(Info + 24) || Word(Info + 28) || Word(Info + 32) != 1 ||
      Layout.Tables[2].EndVA - TryMap != 20 ||
      Layout.Tables[3].EndVA - HandlerMap != 16 * uint64_t(Catches.size()) ||
      Word(TryMap + 12) != Catches.size() || Word(TryMap + 16) != HandlerMap)
    return rejectCxx("raw FuncInfo or try-map closure changed");
  const int32_t TryLow = int32_t(Word(TryMap));
  const int32_t TryHigh = int32_t(Word(TryMap + 4));
  const int32_t CatchHigh = int32_t(Word(TryMap + 8));
  if (TryLow < 0 || TryLow > TryHigh || TryHigh >= CatchHigh ||
      CatchHigh >= int32_t(MaxState))
    return rejectCxx("generated try interval is malformed");
  COFFRegistrationCxxTableReceipt Receipt;
  Receipt.OwnerVA = *Root;
  Receipt.FuncInfoVA = Info;
  for (unsigned I = 0; I != 3; ++I)
    Receipt.Tables[I] = {Layout.Tables[I].BeginVA, Layout.Tables[I].EndVA};
  auto Pointer = [&](va_t Field, llvm::StringRef Symbol, va_t Target) {
    const auto *Section = coff_registration::sectionAt(
        Compiled, Field, 4, SectionKind::ReadOnlyData);
    if (!Section || Word(Field) != Target ||
        !coff_registration::exactPointerFixup(*Section, Field, Symbol, Target))
      return false;
    Receipt.AbsolutePointerFields.push_back(Field);
    return true;
  };
  if (!Pointer(Info + 8, Layout.Tables[1].BeginSymbol, Unwind) ||
      !Pointer(Info + 16, Layout.Tables[2].BeginSymbol, TryMap) ||
      !Pointer(TryMap + 16, Layout.Tables[3].BeginSymbol, HandlerMap))
    return rejectCxx("table pointers lost exact compiler fixup ownership");
  std::vector<const llvm::CatchPadInst *> Pads(Catches.size());
  size_t Work = 0;
  for (const auto &Block : Function)
    for (const auto &I : Block)
      if (const auto *Pad = llvm::dyn_cast<llvm::CatchPadInst>(&I)) {
        bool Matched = false;
        for (uint32_t Index = 0; Index < Catches.size(); ++Index) {
          if (++Work > limits::kMaxRegistrationEHStateWork)
            return rejectCxx("catch row matching exceeded its work budget");
          if (coff_registration::exactSemanticToken(*Pad,
                                                    Catches[Index]->Token)) {
            if (Pads[Index])
              return rejectCxx("catch has multiple generated pads");
            Pads[Index] = Pad;
            Matched = true;
          }
        }
        if (!Matched)
          return rejectCxx("catch has no generated source row");
      }
  for (uint32_t Index = 0; Index < Catches.size(); ++Index) {
    const auto *CatchRow = Catches[Index];
    const auto &Catch = Try.Handlers[Index];
    const auto &Layout = *CatchRow->X86CxxLayout;
    const va_t HandlerMap = Layout.Tables[3].BeginVA + uint64_t(Index) * 16;
    if (CatchRow->ContainerVA != TryMap || CatchRow->RecordVA != HandlerMap ||
        !Pointer(HandlerMap + 12, CatchRow->HandlerSymbol, CatchRow->HandlerVA))
      return rejectCxx(
          "catch row lost its ordered table occurrence or handler fixup");
    const auto *Pad = Pads[Index];
    if (!Pad || Pad->arg_size() != 3 ||
        !Function.hasFnAttribute(llvm::RewriteWinX86CxxFrameAttribute))
      return rejectCxx("catch object has no compiler shared-frame contract");
    auto Object = llvm::getRewriteWinX86CxxCatchFrameObject(*Pad);
    if (!Object)
      return Object.takeError();
    const auto *Type = llvm::dyn_cast<llvm::GlobalVariable>(
        Pad->getArgOperand(0)->stripPointerCasts());
    if (Word(HandlerMap) != Catch.Adjectives ||
        Word(HandlerMap + 4) != Catch.TypeDescriptorVA)
      return rejectCxx("catch row changed its type or adjectives");
    const auto *Section = coff_registration::sectionAt(
        Compiled, HandlerMap, 16, SectionKind::ReadOnlyData);
    if (!Section)
      return rejectCxx("catch row has no unique table extent");
    if (Catch.CatchObjectOffset) {
      if (!Object->Frame ||
          int32_t(Word(HandlerMap + 8)) != Layout.Frame[0] + Layout.Frame[2] ||
          Layout.Frame[2] != Object->Offset || Layout.Frame[3] != Object->Size)
        return rejectCxx("catch row changed its object subfield");
      const auto Size = Object->Frame->getAllocationSize(
          Function.getParent()->getDataLayout());
      if (!Size || Size->isScalable() ||
          Size->getFixedValue() != uint64_t(Layout.Frame[1]))
        return rejectCxx("catch row changed its whole-frame allocation");
    } else if (Object->Frame || Object->Offset || Object->Size ||
               Layout.Frame != std::array<int64_t, 4>{} ||
               Word(HandlerMap + 8) ||
               !coff_registration::hasNoFixup(*Section, HandlerMap + 8, 4)) {
      return rejectCxx("unbound catch acquired an object home");
    }
    if (Catch.TypeDescriptorVA) {
      if (!Type || !Type->isDeclaration() || !Type->hasExternalLinkage())
        return rejectCxx("typed catch lost its external RTTI declaration");
      llvm::SmallString<64> TypeSymbol;
      llvm::Mangler Mangler;
      Mangler.getNameWithPrefix(TypeSymbol, Type, false);
      if (!Pointer(HandlerMap + 4, TypeSymbol, Catch.TypeDescriptorVA))
        return rejectCxx("typed catch lost the original RTTI pointer identity");
    } else if (!llvm::isa<llvm::ConstantPointerNull>(Pad->getArgOperand(0)) ||
               !coff_registration::hasNoFixup(*Section, HandlerMap + 4, 4)) {
      return rejectCxx("catch-all acquired a typed RTTI pointer");
    }
  }
  auto &Mapping = Receipt.SourceToGeneratedStates;
  Mapping.emplace(-1, -1);
  Mapping.emplace(Try.TryLow, TryLow);
  if (!Mapping.emplace(Try.CatchHigh, CatchHigh).second)
    return rejectCxx("catch and try states conflict");
  size_t Actions = 0;
  for (uint32_t I = 0; I < Cxx.UnwindMap.size(); ++I) {
    const auto &Action = Cxx.UnwindMap[I];
    if (!Action.ActionVA)
      continue;
    ++Actions;
    const auto Found = BySource.find(I);
    if (Found == BySource.end() || int32_t(I) <= Try.TryLow ||
        int32_t(I) > Try.TryHigh ||
        !Mapping.emplace(I, Found->second->GeneratedState).second)
      return rejectCxx("source cleanup is absent or outside the lowered try");
    const auto &Row = *Found->second;
    if (Row.RecordVA != Unwind + uint64_t(Row.GeneratedState) * 8 ||
        int32_t(Word(Row.RecordVA)) != Row.EnclosingState ||
        !Pointer(Row.RecordVA + 4, Row.HandlerSymbol, Row.HandlerVA))
      return rejectCxx("cleanup bytes disagree with their indexed row");
  }
  if (Actions != BySource.size())
    return rejectCxx("extra cleanup rows have no source unwind action");
  std::function<std::optional<int32_t>(int32_t)> Project =
      [&](int32_t State) -> std::optional<int32_t> {
    const auto Found = Mapping.find(State);
    if (Found != Mapping.end())
      return Found->second;
    if (State <= Try.TryLow || State > Try.TryHigh ||
        uint32_t(State) >= Cxx.UnwindMap.size())
      return std::nullopt;
    const auto &Action = Cxx.UnwindMap[State];
    if (Action.ActionVA || Action.ToState < Try.TryLow ||
        Action.ToState >= State)
      return std::nullopt;
    auto Outer = Project(Action.ToState);
    if (Outer)
      Mapping.emplace(State, *Outer);
    return Outer;
  };
  for (int32_t I = 0; I < int32_t(Cxx.UnwindMap.size()); ++I) {
    const auto State = Project(I);
    if (!State || *State < TryLow || *State >= int32_t(MaxState) ||
        (I <= Try.TryHigh && *State > TryHigh))
      return rejectCxx(
          "source state has no exact generated dispatch projection");
  }
  std::map<int32_t, int32_t> GeneratedSources = {{TryLow, Try.TryLow},
                                                 {CatchHigh, Try.CatchHigh}};
  for (const auto &[SourceState, Row] : BySource)
    if (!GeneratedSources.emplace(Row->GeneratedState, SourceState).second)
      return rejectCxx("cleanup reused another generated dispatch state");
  if (GeneratedSources.size() != MaxState)
    return rejectCxx("generated unwind map contains an unbound state");
  for (uint32_t I = 0; I < MaxState; ++I) {
    if (!GeneratedSources.count(I))
      return rejectCxx("generated unwind state numbering has a hole");
    const int32_t SourceState = GeneratedSources.at(I);
    const auto ExpectedOuter = Project(Cxx.UnwindMap[SourceState].ToState);
    if (!ExpectedOuter || int32_t(Word(Unwind + I * 8)) != *ExpectedOuter ||
        (!ByState.count(I) && Word(Unwind + I * 8 + 4)))
      return rejectCxx(
          "generated unwind edges changed the source action graph");
  }
  llvm::sort(Receipt.AbsolutePointerFields);
  if (std::adjacent_find(Receipt.AbsolutePointerFields.begin(),
                         Receipt.AbsolutePointerFields.end()) !=
      Receipt.AbsolutePointerFields.end())
    return rejectCxx("language pointer fields overlap");
  BinaryImage GeneratedImage;
  GeneratedImage.Arch = Arch::X86;
  GeneratedImage.Bits = Bitness::Bits32;
  GeneratedImage.Format = BinaryFormat::COFF;
  GeneratedImage.Base = Image.Base;
  GeneratedImage.Segments = Image.Segments;
  Segment Generated;
  Generated.VA = Compiled.BaseVA;
  Generated.Size = Compiled.Bytes.size();
  Generated.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Generated.Data = Compiled.Bytes;
  if (Generated.VA > UINT32_MAX ||
      Generated.Size > uint64_t(UINT32_MAX) + 1 - Generated.VA)
    return rejectCxx("generated language image exceeds PE32 storage");
  for (const auto &Original : Image.Segments) {
    if (Original.Size > InvalidVA - Original.VA ||
        ExceptionAddressRange{Original.VA, Original.VA + Original.Size}
            .overlaps({Generated.VA, Generated.VA + Generated.Size}))
      return rejectCxx("generated language image overlaps original storage");
  }
  GeneratedImage.Segments.push_back(std::move(Generated));
  auto Decoded =
      coff_loader::getCheckedX86CxxFuncInfoRecords(GeneratedImage, Info);
  if (!Decoded || !Decoded->HasDistinctRanges ||
      Decoded->Ranges.size() != Layout.Tables.size())
    return rejectCxx(
        "generated FuncInfo has no complete normalized wire graph");
  for (const auto &Table : Layout.Tables)
    if (!llvm::any_of(Decoded->Ranges, [&](const auto &Range) {
          return Range.Begin == Table.BeginVA && Range.End == Table.EndVA;
        }))
      return rejectCxx("normalized generated graph changed an indexed extent");
  Receipt.GeneratedCxxGraph = std::move(Decoded->Cxx);
  return Receipt;
#endif
}
llvm::Expected<COFFRegistrationCxxHandlerReceipt>
getCheckedCOFFRegistrationCxxHandlerReceipt(const llvm::Function &Function,
                                            const ExceptionFunction &Source,
                                            const BinaryImage &Image,
                                            const CompiledImage &Compiled) {
#ifndef LLVM_NEVERD_X86_CXX_HANDLER_RECEIPTS
  return rejectCxx("LLVM does not provide PE32 C++ handler receipts");
#else
  auto Tables = getCheckedCOFFRegistrationCxxTableReceipt(Function, Source,
                                                          Image, Compiled);
  if (!Tables)
    return Tables.takeError();
  const auto Runtime =
      coff_loader::getCheckedX86CxxPersonalityABI(Image, Source);
  const auto Row =
      llvm::find_if(Compiled.WinEHSemanticRecords, [&](const auto &R) {
        return R.SourceFunction == Function.getName() &&
               R.Token.Kind ==
                   llvm::mc_rewrite::RewriteWinEHSemanticKind::CxxCatch;
      });
  if (!Runtime || Row == Compiled.WinEHSemanticRecords.end() ||
      !Row->X86CxxLayout)
    return rejectCxx(
        "registration handler lost source runtime or table identity");
  const auto &Layout = *Row->X86CxxLayout;
  const CompiledFunctionRange *Handler = nullptr, *Parent = nullptr;
  for (const auto &Range : Compiled.FunctionRanges) {
    if (Range.OwnerSymbol == Layout.RegistrationHandlerSymbol) {
      if (Handler)
        return rejectCxx("registration handler has multiple generated ranges");
      Handler = &Range;
    }
    if (Range.OwnerSymbol == Row->OwnerSymbol) {
      if (Parent)
        return rejectCxx("registration parent has multiple generated ranges");
      Parent = &Range;
    }
  }
  if (!Handler || !Parent || Handler->BeginVA != Layout.RegistrationHandlerVA ||
      Handler->EndVA - Handler->BeginVA != 10 ||
      Handler->ParentOwnerSymbol != Row->OwnerSymbol ||
      Handler->ParentOwnerVA != Row->OwnerVA)
    return rejectCxx(
        "registration handler has no exact parent-owned code range");
  using SectionKind = llvm::mc_rewrite::RewriteSectionKind;
  const auto *Code = coff_registration::sectionAt(Compiled, Handler->BeginVA,
                                                  10, SectionKind::Code);
  const auto *Body = coff_registration::sectionAt(
      Compiled, Parent->BeginVA, Parent->EndVA - Parent->BeginVA,
      SectionKind::Code);
  if (!Code || !Body)
    return rejectCxx("registration code has no unique executable placement");
  const uint8_t *Bytes =
      Compiled.Bytes.data() + Handler->BeginVA - Compiled.BaseVA;
  const uint32_t Delta = readLE<uint32_t>(Bytes + 6);
  if (Bytes[0] != 0xb8 || Bytes[5] != 0xe9 ||
      readLE<uint32_t>(Bytes + 1) != Tables->FuncInfoVA ||
      uint32_t(Handler->BeginVA + 10 + Delta) != Runtime->RuntimeVA ||
      !coff_registration::exactPointerFixup(*Code, Handler->BeginVA + 1,
                                            Layout.Tables[0].BeginSymbol,
                                            Tables->FuncInfoVA))
    return rejectCxx(
        "handler changed its FuncInfo load or original CRT branch");
  const auto *Personality =
      Function.hasPersonalityFn()
          ? llvm::dyn_cast<llvm::Function>(
                Function.getPersonalityFn()->stripPointerCasts())
          : nullptr;
  if (!Personality)
    return rejectCxx("handler lost its exact personality symbol");
  auto RuntimeIdentity = rewrite_source::getOriginalVA(*Personality);
  if (!RuntimeIdentity)
    return RuntimeIdentity.takeError();
  if (*RuntimeIdentity != Runtime->RuntimeVA || !Personality->isDeclaration() ||
      !Personality->hasExternalLinkage())
    return rejectCxx("handler personality lost its original CRT identity");
  llvm::SmallString<64> RuntimeSymbol;
  llvm::Mangler Mangler;
  Mangler.getNameWithPrefix(RuntimeSymbol, Personality, false);
  const CompiledFixupReference *Branch = nullptr;
  const va_t BranchField = Handler->BeginVA + 6;
  size_t Work = 0;
  for (const auto &Fixup : Code->FixupReferences) {
    const uint64_t Width =
        Fixup.BitWidth ? (uint64_t(Fixup.BitWidth) + 7) / 8 : 8;
    if (++Work > limits::kMaxRegistrationEHStateWork ||
        !rangeInBounds(Fixup.Offset, Width, Code->Size))
      return rejectCxx("handler fixup leaves its bounded section");
    const uint64_t Offset = BranchField - Code->VA;
    if (Fixup.Offset < Offset + 4 && Offset < Fixup.Offset + Width) {
      if (Branch || Fixup.Offset != Offset || Fixup.BitWidth != 32 ||
          !Fixup.IsPCRel || !Fixup.IsResolved || Fixup.Specifier ||
          !Fixup.SubtractSymbol.empty() || Fixup.Symbol != RuntimeSymbol ||
          Fixup.Addend || Fixup.KindName != "FK_Data_4" ||
          Fixup.Kind != llvm::FK_Data_4 ||
          uint32_t(Fixup.ResolvedValue) != Delta)
        return rejectCxx("CRT branch lost its exact compiler fixup: kind=" +
                         llvm::Twine(Fixup.Kind) + "/" + Fixup.KindName +
                         " symbol=" + Fixup.Symbol +
                         " expected=" + RuntimeSymbol +
                         " addend=" + llvm::Twine(Fixup.Addend) +
                         " width=" + llvm::Twine(Fixup.BitWidth) +
                         " pcrel=" + llvm::Twine(Fixup.IsPCRel) +
                         " resolved=" + llvm::Twine(Fixup.IsResolved) +
                         " value=" + llvm::Twine(Fixup.ResolvedValue) +
                         " encoded=" + llvm::Twine(Delta));
      Branch = &Fixup;
    }
  }
  if (!Branch)
    return rejectCxx("CRT branch has no compiler fixup");
  const auto &Node = Layout.RegistrationFrame;
  const unsigned Base = Node[0] == 3   ? X86_REG_EBX
                        : Node[0] == 5 ? X86_REG_EBP
                        : Node[0] == 6 ? X86_REG_ESI
                                       : X86_REG_EDI;
  std::set<va_t> ParentPointers;
  for (const auto &Fixup : Body->FixupReferences) {
    if (++Work > limits::kMaxRegistrationEHStateWork)
      return rejectCxx(
          "parent handler pointer proof exhausted its work budget");
    if (!coff_registration::absolutePointer(Fixup) ||
        Fixup.ResolvedValue != Handler->BeginVA)
      continue;
    const va_t Field = Body->VA + Fixup.Offset;
    if (Field < Parent->BeginVA || Field + 4 > Parent->EndVA ||
        !coff_registration::exactPointerFixup(
            *Body, Field, Layout.RegistrationHandlerSymbol, Handler->BeginVA) ||
        !ParentPointers.insert(Field).second)
      return rejectCxx("registration pointer lost its exact parent occurrence");
  }
  if (ParentPointers.size() != 1)
    return rejectCxx("registration parent has no unique handler installation");
  const va_t Field = *ParentPointers.begin();
  Decoder Decode;
  if (!Decode.init(Arch::X86))
    return rejectCxx("registration parent decoder is unavailable");
  bool Installed = false;
  for (va_t Cursor = Parent->BeginVA; Cursor < Parent->EndVA;) {
    DecodedInsn I{};
    if (++Work > limits::kMaxRegistrationEHStateWork ||
        !Decode.decodeOne(Compiled.Bytes.data() + Cursor - Compiled.BaseVA,
                          Parent->EndVA - Cursor, Cursor, I) ||
        !I.Size || I.Size > Parent->EndVA - Cursor || !I.Raw || !I.Raw->detail)
      return rejectCxx("registration parent has no complete decoded body");
    if (Cursor <= Field && Field < Cursor + I.Size) {
      const auto &X = I.Raw->detail->x86;
      if (I.Id != X86_INS_MOV || X.op_count != 2 ||
          X.operands[0].type != X86_OP_MEM || X.operands[0].size != 4 ||
          X.operands[0].mem.segment != X86_REG_INVALID ||
          X.operands[0].mem.base != Base ||
          X.operands[0].mem.index != X86_REG_INVALID ||
          X.operands[0].mem.disp != Node[1] + 8 ||
          X.operands[1].type != X86_OP_IMM || X.encoding.imm_size != 4 ||
          Cursor + X.encoding.imm_offset != Field ||
          uint32_t(X.operands[1].imm) != Handler->BeginVA)
        return rejectCxx("handler pointer is not stored to its compiler node");
      Installed = true;
    }
    Cursor += I.Size;
  }
  if (!Installed)
    return rejectCxx("registration handler store has no decoded occurrence");
  size_t SafeRows = 0;
  for (const auto &Section : Compiled.Sections) {
    if (Section.Name != ".sxdata")
      continue;
    if (Section.IsAllocated || Section.IsInImage || Section.Size % 4 ||
        Section.ExternalBytes.size() != Section.Size ||
        Section.SymbolIndexReferences.size() != Section.Size / 4)
      return rejectCxx("registration handler has malformed SafeSEH metadata");
    std::set<uint64_t> Offsets;
    for (const auto &Reference : Section.SymbolIndexReferences) {
      if (++Work > limits::kMaxRegistrationEHStateWork ||
          Reference.Offset % 4 ||
          !rangeInBounds(Reference.Offset, 4, Section.Size) ||
          !Offsets.insert(Reference.Offset).second)
        return rejectCxx("SafeSEH row has no bounded unique occurrence");
      if (Reference.Symbol == Layout.RegistrationHandlerSymbol ||
          Reference.TargetVA == Handler->BeginVA) {
        if (Reference.Symbol != Layout.RegistrationHandlerSymbol ||
            Reference.TargetVA != Handler->BeginVA || ++SafeRows != 1)
          return rejectCxx("SafeSEH changed registration handler identity");
      }
    }
  }
  if (SafeRows != 1)
    return rejectCxx("registration handler is absent from compiler SafeSEH");
  COFFRegistrationCxxHandlerReceipt Receipt;
  Receipt.Tables = std::move(*Tables);
  Receipt.CodeRange = {Handler->BeginVA, Handler->EndVA};
  Receipt.AbsolutePointerFields = Receipt.Tables.AbsolutePointerFields;
  Receipt.AbsolutePointerFields.push_back(Handler->BeginVA + 1);
  Receipt.AbsolutePointerFields.push_back(Field);
  llvm::sort(Receipt.AbsolutePointerFields);
  return Receipt;
#endif
}
} // namespace neverd
