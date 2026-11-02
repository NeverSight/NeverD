//===- SourcePatchPreparation.cpp - Source installation preparation ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Prepare and reread callback storage before compiling replacement references.
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/SourcePatchPreparation.h"

#include "MachO/MachODependencyState.h"

#include "neverd/backend/Limits.h"
#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/codegen/COFF/COFFSourceImage.h"
#include "neverd/backend/codegen/ELF/ELFARMEHABIPatch.h"
#include "neverd/backend/codegen/ELF/ELFExceptionPatch.h"
#include "neverd/backend/codegen/MachO/MachOCompactUnwindPatch.h"
#include "neverd/backend/codegen/MachO/MachOExceptionPatch.h"
#include "neverd/backend/codegen/MachO/MachOLayoutGrowth.h"
#include "neverd/backend/codegen/MachO/MachOPatch.h"
#include "neverd/loader/Loader.h"
#include "neverd/loader/MachO/MachOARM32Mode.h"
#include "neverd/object/PELayout.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/SHA256.h"

#include <capstone/capstone.h>
#include <set>

namespace neverd {
namespace {
#define NEVERD_SOURCE_PATCH_PREPARATION_DIAGNOSTIC(Name, Value)                \
  constexpr llvm::StringLiteral Name = Value;
#include "SourcePatchPreparation.def"

llvm::Error fail(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::errc::not_supported,
                                 "source preparation: " + Message);
}

llvm::Error verifyBodyEdges(const BinaryImage &Image,
                            const SourceCallbackPrefix &Prefix, uint64_t End) {
  if (End <= Prefix.OriginalVA + Prefix.CopiedSize ||
      End - Prefix.OriginalVA >
          ::neverd::limits::backend::kMaxSourcePatchPrefixBytes)
    return fail("callback needs a bounded complete original function body");
  const auto *Bytes = Image.readVA(Prefix.OriginalVA, End - Prefix.OriginalVA);
  if (!Bytes)
    return fail("original callback body is not file-backed");
  const cs_arch Arch = Image.Arch == Arch::AArch64 ? CS_ARCH_AARCH64
                       : Image.Arch == Arch::ARM   ? CS_ARCH_ARM
                                                   : CS_ARCH_X86;
  const cs_mode Mode = Image.Arch == Arch::X64   ? CS_MODE_64
                       : Image.Arch == Arch::X86 ? CS_MODE_32
                       : Prefix.Mode == InstructionMode::Thumb
                           ? static_cast<cs_mode>(CS_MODE_THUMB | CS_MODE_V8)
                           : CS_MODE_ARM;
  csh Handle = 0;
  if (cs_open(Arch, Mode, &Handle) != CS_ERR_OK)
    return fail("cannot decode callback control-flow boundaries");
  auto Close = llvm::scope_exit([&] { cs_close(&Handle); });
  cs_option(Handle, CS_OPT_DETAIL, CS_OPT_ON);
  cs_insn *I = cs_malloc(Handle);
  if (!I)
    return fail("cannot allocate callback body decoder");
  auto Free = llvm::scope_exit([&] { cs_free(I, 1); });
  size_t Left = End - Prefix.OriginalVA;
  uint64_t PC = Prefix.OriginalVA;
  while (Left) {
    if (Image.Arch == Arch::ARM && Image.instructionModeAt(PC) != Prefix.Mode)
      return fail("callback body crosses a data or instruction-mode boundary");
    if (!cs_disasm_iter(Handle, &Bytes, &Left, &PC, I) || !I->detail)
      return fail("callback body contains an undecodable instruction");
    const bool Call = cs_insn_group(Handle, I, CS_GRP_CALL);
    const bool Jump = cs_insn_group(Handle, I, CS_GRP_JUMP);
    if ((!Call && !Jump) || cs_insn_group(Handle, I, CS_GRP_RET))
      continue;
    std::optional<uint64_t> Target;
    if (Image.Arch == Arch::X86 || Image.Arch == Arch::X64) {
      const auto &Operands = I->detail->x86;
      if (Operands.op_count == 1 && Operands.operands[0].type == X86_OP_IMM)
        Target = Operands.operands[0].imm;
    } else if (Image.Arch == Arch::AArch64) {
      const auto &Operands = I->detail->aarch64;
      for (unsigned N = 0; N < Operands.op_count; ++N)
        if (Operands.operands[N].type == AARCH64_OP_IMM)
          Target = Operands.operands[N].imm;
    } else {
      const auto &Operands = I->detail->arm;
      if (I->id == ARM_INS_BX && Operands.op_count == 1 &&
          Operands.operands[0].type == ARM_OP_REG &&
          Operands.operands[0].reg == ARM_REG_LR)
        continue;
      for (unsigned N = 0; N < Operands.op_count; ++N)
        if (Operands.operands[N].type == ARM_OP_IMM)
          Target = uint64_t(Operands.operands[N].imm) & ~uint64_t(1);
    }
    if (Jump && !Target)
      return fail("indirect jump may re-enter the overwritten original prefix");
    if (Target && *Target >= Prefix.OriginalVA &&
        *Target < Prefix.OriginalVA + Prefix.CopiedSize &&
        !(Call && *Target == Prefix.OriginalVA))
      return fail("original body has an edge into the relocated entry prefix");
  }
  return llvm::Error::success();
}
} // namespace

llvm::Expected<SourcePatchPreparation>
prepareSourcePatchCallbacks(const BinaryImage &Original,
                            llvm::ArrayRef<SourcePatchMapping> Mappings,
                            llvm::ArrayRef<SourcePatchBinding> Bindings) {
  const bool Windows = Original.Format == BinaryFormat::COFF;
  const bool Apple = Original.Format == BinaryFormat::MachO;
  bool AppleSJLJ = false;
  if (Apple && Original.Arch == Arch::ARM) {
    auto Modes = macho_arm32::parseModeInfo(Original.Raw);
    if (!Modes)
      return Modes.takeError();
    AppleSJLJ = Modes->CPUSubtype != llvm::MachO::CPU_SUBTYPE_ARM_V7K;
  }
  const bool ARMELF = Original.isELF() && Original.Arch == Arch::ARM;
  if (!Windows && !Apple && Original.Format != BinaryFormat::ELF)
    return fail("original callbacks require a modeled native unwind installer");
  const bool WindowsX86 = Windows && Original.Arch == Arch::X86;
  auto Region = findELFEHFrameRegion(Original.Raw);
  const auto ARMRegion = findELFARMEHABIRegion(Original.Raw);
  if (ARMELF && !ARMRegion)
    return fail("callback needs a native ARM unwind index");
  if (!Windows && !Apple && !ARMELF && (!Region || !Region->HasHdr))
    return fail(
        "original callbacks require a published DWARF frame search table");
  auto Patcher = BinaryPatcher::create(Original.Format);
  if (!Patcher)
    return fail("no native format installer");
  Patcher->setInputKind(RewriteInputKind::ReplacementIR);
  auto Bytes = Original.Raw;
  if (Apple) {
    auto Compact = findMachOCompactUnwindRegion(Bytes);
    if (!Compact)
      return Compact.takeError();
    if (!*Compact && !Original.ExceptionMetadata.Functions.empty()) {
      std::vector<NativeMachOCompactRange> Existing;
      for (const auto &F : Original.ExceptionMetadata.Functions) {
        if (!F.Dwarf || F.ParseStatus != ExceptionParseStatus::Complete ||
            F.Dwarf->SectionOffset > 0xffffff)
          return fail("original frame cannot join the compact registry");
        const uint32_t Kind =
            Original.Arch == Arch::AArch64 ? 0x03000000 : 0x04000000;
        Existing.push_back({F.CodeRange.Begin, F.CodeRange.End,
                            Kind | uint32_t(F.Dwarf->SectionOffset)});
      }
      if (auto E =
              installNativeMachOCompactUnwind(Bytes, Original.Arch, Existing))
        return E;
    }
    const uint64_t Count = llvm::count_if(Bindings, [](const auto &B) {
      return B.Kind == SourcePatchBindingKind::OriginalFunction;
    });
    auto Grown =
        reserveMachOSourceUnwind(Bytes, Original.Arch, Count * 1024, Count);
    if (!Grown)
      return Grown.takeError();
  }
  const auto AppleEH = Apple ? findMachOEHFrameRegion(Bytes) : std::nullopt;
  if (Apple && !AppleEH)
    return fail("callback needs a registered Mach-O DWARF section");
  const uint64_t Base = Patcher->plannedExecSegmentVA(Bytes, Original.Arch);
  if (!Base)
    return fail("cannot reserve callback code storage");
  SourcePatchPreparation Result;
  Result.OriginalSHA256 = llvm::SHA256::hash(Original.Raw);
  Result.Bindings.assign(Bindings.begin(), Bindings.end());
  CompiledImage Storage;
  Storage.BaseVA = Base;
  Storage.TargetArch = Original.Arch;
  Storage.Success = true;
  std::set<std::string> Names;
  std::set<uint64_t> Entries;
  std::vector<uint64_t> Required;
  for (auto &Binding : Result.Bindings) {
    if (Binding.Kind != SourcePatchBindingKind::OriginalFunction)
      continue;
    const uint64_t VA = Binding.OriginalVA;
    if (Binding.Symbol.empty() ||
        Binding.Symbol.find('\0') != std::string::npos || Binding.Size ||
        !Names.insert(Binding.Symbol).second || !Entries.insert(VA).second ||
        llvm::count_if(
            Mappings, [&](const auto &M) { return M.OriginalVA == VA; }) != 1 ||
        !Original.hasAuthenticatedFunctionEntryAt(VA) ||
        !sourceEntryCanHostTrampoline(Original, VA))
      return fail(
          "original binding must name one distinct replaced function entry");
    const auto *Function = Original.ExceptionMetadata.findFunction(VA);
    if (Apple && Function && Function->Compact &&
        Function->Compact->ReturnAddressSigningOffset.has_value())
      return fail(kARM64PAuthLRUnsupportedCallback);
    uint64_t End = Function ? Function->CodeRange.End
                            : Original.getFunctionMetadataEnd(VA);
    const bool CoalescedCompact =
        Apple && Function && Function->Compact && !Function->Dwarf &&
        Function->Compact->Kind != CompactUnwindKind::DwarfFDE &&
        Function->Compact->Kind != CompactUnwindKind::None &&
        Function->Compact->Kind != CompactUnwindKind::Unknown &&
        Function->Compact->SemanticStatus ==
            CompactUnwindSemanticStatus::Complete &&
        Function->CodeRange.Begin <= VA && VA < Function->CodeRange.End;
    if (((WindowsX86 || AppleSJLJ) && End == InvalidVA) || CoalescedCompact) {
      // x86 PE uses registration code rather than a native function table.
      // A later typed entry bounds the conservative body scan, including any
      // intervening alignment instructions. Never infer an end from exports.
      for (const auto &Symbol : Original.Symbols)
        if (Symbol.IsFunc && Symbol.Addr > VA && Symbol.Addr < End &&
            Original.getSegmentFor(Symbol.Addr - 1) ==
                Original.getSegmentFor(VA))
          End = Symbol.Addr;
    }
    if ((!WindowsX86 && !AppleSJLJ &&
         (!Function ||
          Function->ParseStatus != ExceptionParseStatus::Complete ||
          (!Windows && !Apple && !ARMELF && !Function->Dwarf) ||
          (Function->CodeRange.Begin != VA && !CoalescedCompact))) ||
        End == InvalidVA || End <= VA)
      return fail(
          "callback has no complete original entry frame or body bound: "
          "entry=" +
          llvm::Twine(VA) + ", frame=" +
          llvm::Twine(Function ? Function->CodeRange.Begin : 0) + ", status=" +
          llvm::Twine(Function ? unsigned(Function->ParseStatus) : 255) +
          ", end=" + llvm::Twine(End));
    if (!Windows && Function && Function->Dwarf &&
        !Original.ExceptionMetadata.findCIE(Function->Dwarf->CIESectionOffset))
      return fail("callback has no authenticated original CIE");
    const auto Mode = Original.instructionModeAt(VA);
    if (!Mode)
      return fail("original entry has no instruction mode");
    const size_t Available = std::min<uint64_t>(
        ::neverd::limits::backend::
            kSourcePatchPreparationPrepareSourcePatchCallbacksAvailableMinArgument1,
        End - VA);
    const uint8_t *Bytes = Original.readVA(VA, Available);
    if (!Bytes)
      return fail("original prefix is not file-backed");
    Storage.Bytes.resize(llvm::alignTo(Storage.Bytes.size(), 16), 0);
    auto Prefix =
        relocateSourceCallbackPrefix(Original.Arch, *Mode, {Bytes, Available},
                                     VA, Base + Storage.Bytes.size());
    if (!Prefix)
      return Prefix.takeError();
    for (uint64_t I = 1; I < Prefix->CopiedSize; ++I)
      if (!(Original.Arch == Arch::ARM && ((VA + I) & ~uint64_t(1)) == VA) &&
          Original.hasAuthenticatedFunctionEntryAt(VA + I))
        return fail("callback prefix overlaps another authenticated entry");
    // A loader write must be transferred, never left pointing at the new jump.
    // Refuse until the format can migrate that exact relocation occurrence.
    const auto Overlap = [&](uint64_t Field) {
      return Field < VA + Prefix->CopiedSize &&
             (Field >= VA || VA - Field < 16);
    };
    for (const auto &Reloc : Original.Relocations)
      if (Overlap(Reloc.Address))
        return fail("callback prefix needs loader relocation migration");
    for (const auto &Reloc : Original.BaseRelocations)
      if (Overlap(Reloc.Address))
        return fail("callback prefix needs base relocation migration");
    if (auto Error = verifyBodyEdges(Original, *Prefix, End))
      return Error;
    Storage.Bytes.insert(Storage.Bytes.end(), Prefix->Bytes.begin(),
                         Prefix->Bytes.end());
    Required.push_back(Prefix->CallbackVA);
    Binding.Kind = SourcePatchBindingKind::Function;
    Binding.OriginalVA = Prefix->CallbackVA;
    Result.Callbacks.push_back(
        {Binding.Symbol, std::move(*Prefix), {}, 0, Windows});
  }
  if (Result.Callbacks.empty())
    return fail("no original-body binding was requested");
  using Kind = llvm::mc_rewrite::RewriteSectionKind;
  auto AddSection = [&](llvm::StringRef Name, uint64_t Offset, uint64_t Size,
                        Kind K) {
    CompiledSection Section;
    Section.Name = Name.str();
    Section.Offset = Offset;
    Section.VA = Base + Offset;
    Section.Size = Size;
    Section.IsAllocated = Section.IsInImage = true;
    Section.Kind = K;
    Storage.Sections.push_back(std::move(Section));
  };
  AddSection(".text", 0, Storage.Bytes.size(), Kind::Code);
  Storage.Bytes.resize(llvm::alignTo(Storage.Bytes.size(), 8), 0);
  const uint64_t FrameOffset = Storage.Bytes.size();
  std::vector<uint8_t> AppleFrames;
  for (auto &Callback : Result.Callbacks) {
    if (WindowsX86)
      continue;
    const auto *Function =
        Original.ExceptionMetadata.findFunction(Callback.Prefix.OriginalVA);
    Callback.FrameVA = Apple ? AppleEH->AppendVA + AppleFrames.size()
                             : Base + Storage.Bytes.size();
    auto Frame = Windows  ? encodeSourceCallbackCOFFUnwind(
                                Callback.Prefix, *Function, Original.Arch)
                 : ARMELF ? encodeSourceCallbackEHABI(Callback.Prefix)
                 : Apple && (!Function || !Function->Dwarf)
                     ? encodeSourceCallbackMachineDwarf(
                           Callback.Prefix, Original.Arch, Callback.FrameVA)
                     : encodeSourceCallbackDwarfFrame(
                           Callback.Prefix,
                           *Original.ExceptionMetadata.findCIE(
                               Function->Dwarf->CIESectionOffset),
                           *Function->Dwarf, Callback.FrameVA);
    if (!Frame)
      return Frame.takeError();
    Callback.Frame = std::move(*Frame);
    auto &FrameBytes = Apple ? AppleFrames : Storage.Bytes;
    FrameBytes.insert(FrameBytes.end(), Callback.Frame.begin(),
                      Callback.Frame.end());
  }
  if (!Apple)
    AddSection(Windows  ? ".xdata"
               : ARMELF ? ".ARM.extab"
                        : ".eh_frame",
               FrameOffset, Storage.Bytes.size() - FrameOffset,
               Kind::ReadOnlyData);
  if (Apple) {
    CompiledSection Frames;
    Frames.Name = "__eh_frame";
    Frames.VA = AppleEH->AppendVA;
    Frames.Size = AppleFrames.size();
    Frames.Kind = Kind::ReadOnlyData;
    Frames.IsAllocated = true;
    Frames.IsInImage = false;
    Frames.ExternalBytes = AppleFrames;
    if (auto E = installNativeMachOEHFrame(Bytes, *AppleEH, Frames))
      return E;
    if (Patcher->appendExecSegment(Bytes, Storage.Bytes, "__NDORIG",
                                   Original.Arch) != Base)
      return fail("Mach-O callback placement changed during installation");
    auto Records = decodeDwarfEHFrameRecords(AppleFrames, AppleEH->AppendVA,
                                             AppleEH->Is64);
    if (!Records)
      return Records.takeError();
    std::vector<NativeMachOCompactRange> Ranges;
    auto OriginalCompact = findMachOCompactUnwindRegion(Bytes);
    if (!OriginalCompact)
      return OriginalCompact.takeError();
    if (!*OriginalCompact) {
      // Introducing a compact search table must retain the original DWARF
      // functions as well as the callbacks; otherwise source-range mapping
      // and the native runtime see an incomplete registry.
      for (const auto &F : Original.ExceptionMetadata.Functions) {
        if (!F.Dwarf || F.ParseStatus != ExceptionParseStatus::Complete ||
            F.Dwarf->SectionOffset > 0xffffff)
          return fail("original frame cannot join the compact registry");
        const uint32_t Kind =
            Original.Arch == Arch::AArch64 ? 0x03000000 : 0x04000000;
        Ranges.push_back({F.CodeRange.Begin, F.CodeRange.End,
                          Kind | uint32_t(F.Dwarf->SectionOffset)});
      }
    }

    for (const auto &F : *Records) {
      const uint64_t Offset = F.RecordVA - AppleEH->SectionVA;
      if (Offset > 0xffffff)
        return fail("callback FDE exceeds compact unwind reach");
      const uint32_t Kind =
          Original.Arch == Arch::AArch64 ? 0x03000000 : 0x04000000;
      Ranges.push_back({F.BeginVA, F.EndVA, Kind | uint32_t(Offset)});
    }
    if (auto E = installNativeMachOCompactUnwind(Bytes, Original.Arch, Ranges))
      return E;
  } else if (Windows) {
    const uint64_t TableOffset = Storage.Bytes.size();
    std::vector<COFFOriginalCallback> Evidence;
    for (const auto &Callback : Result.Callbacks) {
      const auto &Prefix = Callback.Prefix;
      const uint64_t Begin = Prefix.CallbackVA - Original.Base;
      const uint64_t End = Begin + Prefix.Bytes.size();
      const uint64_t Frame = WindowsX86 ? 0 : Callback.FrameVA - Original.Base;
      if (End > UINT32_MAX || Frame > UINT32_MAX)
        return fail("callback runtime function exceeds PE address limits");
      auto Word = [&](uint32_t Value) {
        const size_t Offset = Storage.Bytes.size();
        Storage.Bytes.resize(Offset + 4);
        writeLE<uint32_t>(Storage.Bytes.data() + Offset, Value);
      };
      if (!WindowsX86) {
        Word(Begin | (Original.Arch == Arch::ARM ? 1u : 0u));
        if (Original.Arch == Arch::X64)
          Word(End);
        Word(Frame);
      }
      Evidence.push_back({Prefix, Callback.FrameVA});
    }
    AddSection(".pdata", TableOffset, Storage.Bytes.size() - TableOffset,
               Kind::ReadOnlyData);
    if (auto Error =
            installCOFFCallbackImage(Bytes, Original, Storage, Evidence))
      return Error;
  } else {
    if (ARMELF) {
      const uint64_t IndexOffset = Storage.Bytes.size();
      for (const auto &C : Result.Callbacks) {
        const uint64_t At = Base + Storage.Bytes.size();
        const int64_t Code = int64_t(C.Prefix.CallbackVA) - int64_t(At);
        const int64_t Frame = int64_t(C.FrameVA) - int64_t(At + 4);
        if (!llvm::isInt<31>(Code) || !llvm::isInt<31>(Frame))
          return fail("ARM callback table exceeds prel31 reach");
        const size_t N = Storage.Bytes.size();
        Storage.Bytes.resize(N + 8);
        writeLE<uint32_t>(Storage.Bytes.data() + N,
                          uint32_t(Code) & 0x7fffffff);
        writeLE<uint32_t>(Storage.Bytes.data() + N + 4,
                          uint32_t(Frame) & 0x7fffffff);
      }
      AddSection(".ARM.exidx", IndexOffset, Storage.Bytes.size() - IndexOffset,
                 Kind::ReadOnlyData);
    }
    const uint64_t HeaderVA = Base + Storage.Bytes.size();
    auto Header =
        ARMELF ? buildRelocatedELFARMEHABIIndex(Original.Raw, *ARMRegion,
                                                Storage, HeaderVA, Required)
               : buildRelocatedELFEHFrameHdr(Original.Raw, *Region, Storage,
                                             Required, HeaderVA);
    if (!Header)
      return Header.takeError();
    Storage.Bytes.insert(Storage.Bytes.end(), Header->begin(), Header->end());
    if (Patcher->appendExecSegment(Bytes, Storage.Bytes, ".ndorig",
                                   Original.Arch) != Base)
      return fail("callback placement changed during installation");
    auto Error = ARMELF
                     ? retargetELFARMEHABIIndex(Bytes, HeaderVA, Header->size())
                     : retargetELFEHFrameHdr(Bytes, HeaderVA, Header->size());
    if (Error)
      return Error;
  }
  if (Original.Arch == Arch::ARM && (Apple || ARMELF)) {
    std::vector<SourceCallbackPrefix> Prefixes;
    for (const auto &C : Result.Callbacks)
      Prefixes.push_back(C.Prefix);
    if (auto E =
            registerSourceCallbackSymbols(Bytes, Original.Format, Prefixes))
      return E;
  }
  auto Reader = Loader::create(Original.Format);
  auto Reloaded = Reader->loadBuffer(llvm::MemoryBufferRef(
      llvm::StringRef(reinterpret_cast<const char *>(Bytes.data()),
                      Bytes.size()),
      "prepared-source-patch"));
  if (!Reloaded)
    return Reloaded.takeError();
  Result.Image = std::move(*Reloaded);
  if (auto Error = verifySourcePatchPreparation(Original, Result, Result.Image))
    return Error;
  return Result;
}

llvm::Error verifySourcePatchPreparation(const BinaryImage &Original,
                                         const SourcePatchPreparation &Prepared,
                                         const BinaryImage &Final) {
  if (llvm::SHA256::hash(Original.Raw) != Prepared.OriginalSHA256 ||
      Final.Format != Original.Format || Final.Arch != Original.Arch ||
      Final.Base != Original.Base)
    return fail("prepared image does not belong to this original input");
  if (auto E = verifySourcePatchDependencies(Final, Prepared.Imports))
    return E;
  if (Original.isMachO() && !Prepared.Imports.empty()) {
    auto Before = readMachODependencyState(Original.Raw);
    if (!Before)
      return Before.takeError();
    auto After = readMachODependencyState(Final.Raw);
    if (!After)
      return After.takeError();
    if (auto E = preserveMachODependencyState(*Before, *After))
      return E;
  }
  for (const auto &Old : Prepared.Image.Imports) {
    if (Final.isELF() && !Old.IATAddr) {
      if (!llvm::is_contained(Final.DynInfo.NeededLibs, Old.Module))
        return fail(
            "original dependency library disappeared before publication");
      continue;
    }
    const auto *Now = Final.findImportAt(Old.IATAddr);
    if (!Now || Now->Name != Old.Name || Now->Module != Old.Module ||
        Now->Ordinal != Old.Ordinal || Now->IsDelayImport != Old.IsDelayImport)
      return fail("native import identity changed before publication");
  }
  for (const auto &Callback : Prepared.Callbacks) {
    const auto &Prefix = Callback.Prefix;
    const uint8_t *Code = Final.readVA(Prefix.CallbackVA, Prefix.Bytes.size());
    const uint8_t *Frame =
        Final.readVA(Callback.FrameVA, Callback.Frame.size());
    const auto *Segment = Final.getSegmentFor(Prefix.CallbackVA);
    const auto *Unwind =
        Final.ExceptionMetadata.findFunction(Prefix.CallbackVA);
    if (!Code || (!Frame && !Callback.Frame.empty()) || !Segment)
      return fail("callback storage is absent from the reloaded image");
    if (!Segment->isExecutable() || Segment->isWritable())
      return fail("callback storage does not have RX permissions");
    if (!std::equal(Prefix.Bytes.begin(), Prefix.Bytes.end(), Code) ||
        (!Callback.Frame.empty() &&
         !std::equal(Callback.Frame.begin(), Callback.Frame.end(), Frame)))
      return fail("callback bytes differ from the preparation receipt");
    if (Final.Format == BinaryFormat::COFF && Final.Arch == Arch::X86) {
      if (!Final.hasAuthenticatedFunctionEntryAt(Prefix.CallbackVA) ||
          !Callback.Frame.empty())
        return fail("x86 callback lost its native code identity");
      continue;
    }
    if (!Unwind || Unwind->ParseStatus != ExceptionParseStatus::Complete ||
        Unwind->CodeRange.Begin != Prefix.CallbackVA ||
        Unwind->CodeRange.End != Prefix.CallbackVA + Prefix.Bytes.size())
      return fail("callback has no complete reloaded native frame");
    if (Callback.WindowsUnwind) {
      if (Unwind->UnwindInfoVA != Callback.FrameVA ||
          Unwind->Personality != ExceptionPersonality::None)
        return fail("callback Windows unwind receipt disagrees");
    } else if (Final.isELF() && Final.Arch == Arch::ARM) {
      if (!Unwind->ARMEHABI ||
          Unwind->ARMEHABI->TableEntryVA != Callback.FrameVA)
        return fail("callback ARM unwind receipt disagrees");
    } else if (!Unwind->Dwarf ||
               Unwind->Dwarf->InitialLocation != Prefix.CallbackVA ||
               Unwind->Dwarf->AddressRange != Prefix.Bytes.size()) {
      return fail("callback DWARF unwind receipt disagrees");
    }
  }
  return llvm::Error::success();
}
} // namespace neverd
