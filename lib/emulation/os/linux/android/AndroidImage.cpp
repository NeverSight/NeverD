//===- AndroidImage.cpp - Bounded AArch64 shared library linking ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AndroidInternal.h"

#include "neverd/emulation/ImageMapping.h"
#include "neverd/loader/BinaryImageModel.h"

#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"

namespace neverd::emulation::android_model {
using namespace llvm::ELF;
namespace {
llvm::Error put64(GuestMemory &Memory, uint64_t Address, uint64_t Value) {
  uint8_t Bytes[8];
  llvm::support::endian::write64le(Bytes, Value);
  return Memory.writeBacking(Address, Bytes);
}
llvm::Expected<uint64_t> get64(GuestMemory &Memory, uint64_t Address) {
  uint8_t Bytes[8];
  if (auto E = Memory.readBacking(Address, Bytes))
    return std::move(E);
  return llvm::support::endian::read64le(Bytes);
}
} // namespace
llvm::Expected<LinkedImage> loadImage(AddressSpace &Space,
                                      const BinaryImage &Image,
                                      const ProcessOptions &Options) {
  const auto &Native = *Options.Android;
  if (!Image.isELF() || Image.Arch != Arch::AArch64 ||
      Image.Bits != Bitness::Bits64 || !Image.ELFMetadata ||
      Image.ELFMetadata->Type != ET_DYN)
    return failure("requires an AArch64 ELF64LE shared library");
  for (const auto &P : Image.ELFMetadata->ProgramHeaders) {
    if (P.Type == PT_INTERP || (P.Type == PT_TLS && P.MemorySize))
      return failure("interpreters and ELF TLS templates are unsupported");
    if (P.Type == PT_GNU_STACK && (P.Flags & PF_X))
      return failure("executable stacks are unsupported");
    if (P.Type == PT_LOAD) {
      if (P.FileSize > P.MemorySize || P.FileOffset > Image.Raw.size() ||
          P.FileSize > Image.Raw.size() - P.FileOffset ||
          P.VirtualAddress % PageSize != P.FileOffset % PageSize ||
          (P.Flags & ~(PF_R | PF_W | PF_X)))
        return failure(
            "invalid Android PT_LOAD extent, alignment or permissions");
      if (P.Alignment > 1 &&
          (!llvm::isPowerOf2_64(P.Alignment) ||
           P.VirtualAddress % P.Alignment != P.FileOffset % P.Alignment ||
           Native.LoadBias % P.Alignment))
        return failure("load bias or PT_LOAD violates ELF alignment");
    }
  }
  auto Facts = readELFProgramLinking(Image);
  if (!Facts)
    return Facts.takeError();
  for (const auto &D : Facts->Dynamic)
    if (D.Tag == DT_TEXTREL || (D.Tag == DT_FLAGS && (D.Value & DF_TEXTREL)))
      return failure("Android API 28 disallows text relocations");
  auto Plan = ImageMappingPlan::create(
      Image, Native.LoadBias, PageSize, Options.MemoryLimit - Options.StackSize,
      true, ImagePagePadding::FilePages, ImageByteSource::OriginalFile);
  if (!Plan)
    return Plan.takeError();
  LinkedImage Out{};
  auto Address = [&](uint64_t VA) -> llvm::Expected<uint64_t> {
    if (VA > linux_model::UserLimitARM64 - Native.LoadBias)
      return failure("image address overflows the user address space");
    return Native.LoadBias + VA;
  };
  for (const auto &Region : Plan->Regions) {
    uint64_t End = Region.Address + Region.Bytes.size();
    if (Region.Address < linux_model::MinimumAddress || End > StdioAddress)
      return failure("image overlaps a reserved native runtime range");
    Out.InitialBreak = std::max(Out.InitialBreak, End);
    if (auto E = Space.map(Region.Address, Region.Bytes.size(), Read | Write))
      return std::move(E);
    if (auto E = Space.write(Region.Address, Region.Bytes))
      return std::move(E);
  }
  std::map<std::string, uint64_t> Imported;
  bool NeedsStdio = false;
  auto Symbol = [&](uint32_t Index,
                    uint32_t RelType) -> llvm::Expected<uint64_t> {
    if (!Index)
      return uint64_t(0);
    const auto &S = Facts->Symbols[Index];
    unsigned Type = S.Info & 15;
    if (Type == STT_TLS || Type == STT_GNU_IFUNC ||
        S.SectionIndex == SHN_COMMON || S.SectionIndex == SHN_XINDEX)
      return failure("unsupported dynamic symbol kind: " + S.Name);
    if (S.SectionIndex != SHN_UNDEF)
      return S.SectionIndex == SHN_ABS ? llvm::Expected<uint64_t>(S.Value)
                                       : Address(S.Value);
    if (S.Name == "__sF") {
      NeedsStdio = true;
      return StdioAddress;
    }
    if (S.Name == "__stack_chk_guard")
      return GuardAddress;
    if (Type != STT_FUNC &&
        !(Type == STT_NOTYPE && RelType == R_AARCH64_JUMP_SLOT))
      return failure("unmodeled imported data symbol: " + S.Name);
    auto [I, New] =
        Imported.emplace(S.Name, ThunkBase + 8 * (Imported.size() + 1));
    if (New)
      Out.Imports.emplace(I->second, S.Name);
    return I->second;
  };
  for (const auto &Rel : Facts->Relocations) {
    if (Rel.Type == R_AARCH64_NONE)
      continue;
    if (Rel.Type != R_AARCH64_RELATIVE && Rel.Type != R_AARCH64_ABS64 &&
        Rel.Type != R_AARCH64_GLOB_DAT && Rel.Type != R_AARCH64_JUMP_SLOT)
      return failure("unsupported AArch64 dynamic relocation " +
                     llvm::Twine(Rel.Type));
    auto Dest = Address(Rel.Address);
    if (!Dest)
      return Dest.takeError();
    if (Rel.Type == R_AARCH64_RELATIVE && Rel.Symbol)
      return failure("relative relocation has a symbol");
    uint64_t Addend = static_cast<uint64_t>(Rel.Addend);
    if (!Rel.ExplicitAddend) {
      auto Value = get64(Space, *Dest);
      if (!Value)
        return Value.takeError();
      Addend = *Value;
    }
    auto Base = Rel.Type == R_AARCH64_RELATIVE
                    ? llvm::Expected<uint64_t>(Native.LoadBias)
                    : Symbol(Rel.Symbol, Rel.Type);
    if (!Base)
      return Base.takeError();
    if (auto E = put64(Space, *Dest, *Base + Addend))
      return std::move(E);
  }
  // Each explicit provider gets its own named traps. This is a catalogue of
  // modeled functions, never a request to load a library from the host.
  for (const auto &[Library, Names] : Native.Libraries) {
    auto &Symbols = Out.Libraries[Library];
    for (const auto &Name : Names) {
      uint64_t PC = ThunkBase + 8 * (Out.Imports.size() + 1);
      Out.Imports.emplace(PC, Name);
      Out.DynamicProviders.emplace(PC, Library);
      Symbols.emplace(Name, PC);
    }
  }
  for (const auto &Region : Plan->Regions)
    if (auto E = Space.protect(Region.Address, Region.Bytes.size(),
                               Region.Permissions))
      return std::move(E);
  // FILE storage is deliberately opaque. Address identity is available, but
  // an attempted field access faults instead of observing fabricated bytes.
  if (NeedsStdio)
    if (auto E = Space.map(StdioAddress, PageSize, UserAccessible))
      return std::move(E);
  for (const auto &P : Image.ELFMetadata->ProgramHeaders) {
    if (P.Type != PT_GNU_RELRO || !P.MemorySize)
      continue;
    auto Begin = Address(P.VirtualAddress);
    if (!Begin)
      return Begin.takeError();
    if (*Begin >= TLSAddress || P.MemorySize > TLSAddress - *Begin)
      return failure("invalid RELRO extent");
    uint64_t Start = *Begin & ~(PageSize - 1);
    uint64_t End = (*Begin + P.MemorySize + PageSize - 1) & ~(PageSize - 1);
    if (auto E = Space.protect(Start, End - Start, Read | UserAccessible))
      return std::move(E);
  }
  if (auto E = Space.map(TLSAddress, PageSize, Read | Write | UserAccessible))
    return std::move(E);
  if (auto E = put64(Space, TLSAddress, TLSAddress))
    return std::move(E);
  if (auto E = put64(Space, GuardAddress, StackGuard))
    return std::move(E);
  uint64_t ThunkSize =
      ((Out.Imports.size() + 1) * 8 + PageSize - 1) & ~(PageSize - 1);
  if (auto E = Space.map(ThunkBase, ThunkSize, Read | Write))
    return std::move(E);
  std::vector<uint8_t> Bytes(ThunkSize, 0);
  for (size_t I = 0; I <= Out.Imports.size(); ++I) {
    llvm::support::endian::write32le(Bytes.data() + I * 8,
                                     0xd4000001 | (uint32_t(ModelTrap) << 5));
    llvm::support::endian::write32le(Bytes.data() + I * 8 + 4,
                                     0xd65f03c0); // ret x30
  }
  if (auto E = Space.write(ThunkBase, Bytes))
    return std::move(E);
  if (auto E =
          Space.protect(ThunkBase, ThunkSize, Read | Execute | UserAccessible))
    return std::move(E);
  if (Native.EntryAddress) {
    auto Entry = Address(*Native.EntryAddress);
    if (!Entry)
      return Entry.takeError();
    Out.Entry = *Entry;
  } else {
    for (const auto &S : Facts->Symbols) {
      if (S.Name != Native.EntrySymbol || S.SectionIndex == SHN_UNDEF)
        continue;
      if ((S.Info & 15) != STT_FUNC || S.SectionIndex >= SHN_LORESERVE)
        return failure("entry symbol is not an ordinary defined function");
      auto Entry = Address(S.Value);
      if (!Entry)
        return Entry.takeError();
      if (Out.Entry && Out.Entry != *Entry)
        return failure("ambiguous entry symbol");
      Out.Entry = *Entry;
    }
    if (!Out.Entry)
      return failure("entry symbol was not found: " + Native.EntrySymbol);
  }
  auto Executable = [&](uint64_t PC) -> llvm::Error {
    auto Access = Space.canAccess(PC, 4, Execute | UserAccessible);
    if (!Access)
      return Access.takeError();
    if (PC % 4 || !*Access)
      return failure("entry or constructor is not executable");
    return llvm::Error::success();
  };
  if (auto E = Executable(Out.Entry))
    return std::move(E);
  std::map<int64_t, uint64_t> Tags;
  for (const auto &D : Facts->Dynamic)
    Tags[D.Tag] = D.Value;
  if (Tags[DT_PREINIT_ARRAYSZ])
    return failure("shared library preinit arrays are unsupported");
  if (Native.Initialize) {
    if (Tags[DT_INIT]) {
      auto Init = Address(Tags[DT_INIT]);
      if (!Init)
        return Init.takeError();
      Out.Constructors.push_back(*Init);
    }
    uint64_t Size = Tags[DT_INIT_ARRAYSZ];
    if (Size % 8 || Size > Options.MemoryLimit)
      return failure("invalid initializer array");
    if (Size) {
      auto Start = Address(Tags[DT_INIT_ARRAY]);
      if (!Start)
        return Start.takeError();
      if (auto E = Space.validateBacking(*Start, Size))
        return std::move(E);
      for (uint64_t I = 0; I < Size; I += 8) {
        auto Init = get64(Space, *Start + I);
        if (!Init)
          return Init.takeError();
        if (*Init && *Init != UINT64_MAX)
          Out.Constructors.push_back(*Init);
      }
    }
    for (uint64_t PC : Out.Constructors)
      if (auto E = Executable(PC))
        return std::move(E);
  }
  return Out;
}
} // namespace neverd::emulation::android_model
