//===- COFFRegistrationEH.cpp - x86-32 registration-chain EH -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/COFF/COFFRegistrationEH.h"

#include "neverd/support/BinaryEncoding.h"
#include "neverd/loader/LanguageRuntime.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace neverd::coff_loader {
namespace {

/// Bound on any single decoded table.  A scope table or `FuncInfo` map larger
/// than this is a mis-identification, not a program.
constexpr uint32_t MaxRegistrationRecords = 4096;

/// How far back from an install site the prologue may be searched for the
/// `push` operands.  A frame installs at most a try level, a table pointer,
/// and a handler.
constexpr unsigned MaxInstallPushes = 4;

/// Bound on veneer/thunk chasing when naming a handler.
constexpr unsigned MaxHandlerThunkHops = 4;

/// How far back from a chain-head read the operands of the install may be
/// found.  The record is built immediately before the link, so a window this
/// size covers the widest prologue any of the three shapes below emits while
/// staying far short of the previous function.
constexpr size_t MaxInstallWindow = 64;

/// Recognize a read of the `FS:[0]` chain head.  A frame must read the head
/// before it can link a record in front of it, which makes the read the one
/// instruction every install shape has in common.  Three encodings appear in
/// practice and they differ only in where the value lands:
///
///     64 A1 00 00 00 00      mov  eax, dword ptr fs:[0]     (MSVC)
///     64 8B 0D 00 00 00 00   mov  ecx, dword ptr fs:[0]     (clang-cl)
///     64 FF 35 00 00 00 00   push dword ptr fs:[0]          (LLVM)
///
/// Returns the encoded length, or zero when \p Code does not start with one.
size_t chainHeadReadLength(const uint8_t *Code, size_t Available) {
  if (Available >= 6 && Code[0] == 0x64 && Code[1] == 0xA1 &&
      readLE<uint32_t>(Code + 2) == 0)
    return 6;
  if (Available < 7 || Code[0] != 0x64 || readLE<uint32_t>(Code + 3) != 0)
    return 0;
  // ModRM mod=00 rm=101 is the absolute-displacement form; the reg field
  // selects the destination and does not change the encoding length.
  const bool IsAbsoluteDisp = (Code[2] & 0xC7) == 0x05;
  if (Code[1] == 0x8B && IsAbsoluteDisp)
    return 7;
  if (Code[1] == 0xFF && Code[2] == 0x35)
    return 7;
  return 0;
}

void diagnose(ExceptionFunction &F, ExceptionParseStatus Status,
              const std::string &Message) {
  F.ParseStatus = mergeExceptionParseStatus(F.ParseStatus, Status);
  F.Diagnostics.push_back(Message);
}

bool isExecutableAddress(const BinaryImage &Img, va_t Address) {
  const Segment *Seg = Img.getSegmentFor(Address);
  return Seg && Seg->isExecutable() && Img.readVA(Address, 1) != nullptr;
}

/// Apply a signed instruction displacement without wrapping past the address
/// space, so a corrupt operand cannot fabricate an in-image target.
std::optional<va_t> addSignedOffset(va_t Base, int64_t Displacement) {
  if (Displacement >= 0) {
    if (static_cast<uint64_t>(Displacement) > InvalidVA - Base)
      return std::nullopt;
    return Base + static_cast<uint64_t>(Displacement);
  }
  uint64_t Magnitude = static_cast<uint64_t>(-Displacement);
  if (Magnitude > Base)
    return std::nullopt;
  return Base - Magnitude;
}

template <typename T>
std::optional<T> readScalar(const BinaryImage &Img, va_t Address) {
  const uint8_t *P = Img.readVA(Address, sizeof(T));
  if (!P)
    return std::nullopt;
  return readLE<T>(P);
}

/// Half-open code ranges of every discovered function, sorted by start.  A
/// recovered registration record belongs to whichever function contains its
/// install site; without that attribution the record would have no code range
/// and could not take part in CFG or structuring.
class FunctionRangeMap {
public:
  explicit FunctionRangeMap(const BinaryImage &Img) {
    for (const Symbol &Sym : Img.Symbols) {
      if (!Sym.IsFunc || !isExecutableAddress(Img, Sym.Addr))
        continue;
      Starts.push_back(Sym.Addr);
    }
    std::sort(Starts.begin(), Starts.end());
    Starts.erase(std::unique(Starts.begin(), Starts.end()), Starts.end());
  }

  /// The function containing \p Address, bounded by the next function start
  /// and by the end of the executable segment it lives in.
  std::optional<ExceptionAddressRange> find(const BinaryImage &Img,
                                            va_t Address) const {
    auto It = std::upper_bound(Starts.begin(), Starts.end(), Address);
    if (It == Starts.begin())
      return std::nullopt;
    va_t Begin = *std::prev(It);
    const Segment *Seg = Img.getSegmentFor(Begin);
    if (!Seg || !Seg->isExecutable())
      return std::nullopt;
    uint64_t Usable = std::min<uint64_t>(Seg->Size, Seg->Data.size());
    if (Usable > InvalidVA - Seg->VA)
      return std::nullopt;
    va_t SegEnd = Seg->VA + Usable;
    va_t End = It == Starts.end() ? SegEnd : std::min(*It, SegEnd);
    if (End <= Begin || Address >= End)
      return std::nullopt;
    return ExceptionAddressRange{Begin, End};
  }

private:
  std::vector<va_t> Starts;
};

/// The image's SafeSEH handler table, when the load configuration published
/// one.  It is the authority on which addresses the loader will accept as
/// exception handlers, so a scan hit whose handler is absent from a non-empty
/// table is a false positive rather than an undocumented handler.
class SafeSEHTable {
public:
  explicit SafeSEHTable(const BinaryImage &Img) {
    const va_t ConfigRVA = Img.DynInfo.LoadConfigRVA;
    if (ConfigRVA == 0 || Img.DynInfo.LoadConfigSize < 0x48 ||
        ConfigRVA > InvalidVA - Img.Base)
      return;
    const va_t ConfigVA = Img.Base + ConfigRVA;
    auto TableVA = readScalar<uint32_t>(Img, ConfigVA + 0x40);
    auto Count = readScalar<uint32_t>(Img, ConfigVA + 0x44);
    if (!TableVA || !Count || *TableVA == 0 || *Count == 0 ||
        *Count > MaxRegistrationRecords)
      return;
    for (uint32_t I = 0; I < *Count; ++I) {
      auto Entry = readScalar<uint32_t>(Img, va_t(*TableVA) + uint64_t(I) * 4);
      if (!Entry)
        return;
      if (*Entry > InvalidVA - Img.Base)
        return;
      Handlers.push_back(Img.Base + *Entry);
    }
    std::sort(Handlers.begin(), Handlers.end());
    Present = true;
  }

  bool isPresent() const { return Present; }
  bool contains(va_t Address) const {
    return std::binary_search(Handlers.begin(), Handlers.end(), Address);
  }

private:
  std::vector<va_t> Handlers;
  bool Present = false;
};

/// A 32-bit constant the prologue materialized shortly before linking the
/// record, and therefore a candidate for the handler or the scope table.
struct InstallOperand {
  va_t SiteVA = 0;
  uint32_t Value = 0;
};

/// Collect the 32-bit constants the prologue materializes in the window
/// preceding \p Offset.
///
/// The three install shapes place the handler and table differently — MSVC
/// pushes them, clang-cl stores them into frame slots — so rather than
/// hard-coding one instruction sequence this gathers every constant a
/// prologue could have produced and leaves the choice to validation.  Only the
/// two encodings that can carry an address are scanned, and each is
/// fixed-length from its opcode byte, so a hit is checked by re-deriving its
/// own length rather than by trusting the byte stream to be aligned:
///
///     68 <imm32>                 push  imm32
///     C7 45 <disp8>  <imm32>     mov   dword ptr [ebp+disp8], imm32
///     C7 44 24 <disp8> <imm32>   mov   dword ptr [esp+disp8], imm32
///     C7 85 <disp32> <imm32>     mov   dword ptr [ebp+disp32], imm32
std::vector<InstallOperand> collectInstallOperands(const Segment &Seg,
                                                   size_t Offset) {
  std::vector<InstallOperand> Operands;
  const uint8_t *Data = Seg.Data.data();
  const size_t Start =
      Offset > MaxInstallWindow ? Offset - MaxInstallWindow : 0;
  for (size_t I = Start; I < Offset; ++I) {
    size_t ImmOffset = 0;
    if (Data[I] == 0x68) {
      ImmOffset = I + 1;
    } else if (Data[I] == 0xC7 && I + 1 < Offset) {
      const uint8_t Modrm = Data[I + 1];
      if (Modrm == 0x45)
        ImmOffset = I + 3;
      else if (Modrm == 0x44 && I + 2 < Offset && Data[I + 2] == 0x24)
        ImmOffset = I + 4;
      else if (Modrm == 0x85)
        ImmOffset = I + 6;
      else
        continue;
    } else {
      continue;
    }
    if (ImmOffset + 4 > Seg.Data.size())
      continue;
    Operands.push_back({Seg.VA + I, readLE<uint32_t>(Data + ImmOffset)});
  }
  return Operands;
}

/// The initial try level a prologue seeds, which distinguishes the two SEH
/// scope-table dialects: `_except_handler3` starts at -1 and
/// `_except_handler4` at -2.
std::optional<int32_t> findSeededTryLevel(const Segment &Seg, size_t Offset) {
  const uint8_t *Data = Seg.Data.data();
  const size_t Start =
      Offset > MaxInstallWindow ? Offset - MaxInstallWindow : 0;
  std::optional<int32_t> Level;
  for (size_t I = Start; I < Offset; ++I) {
    // push imm8
    if (Data[I] == 0x6A && I + 1 < Offset) {
      int32_t Value = static_cast<int8_t>(Data[I + 1]);
      if (Value == -1 || Value == -2)
        Level = Value;
      continue;
    }
    // mov dword ptr [ebp+disp8], imm32
    if (Data[I] == 0xC7 && I + 6 < Offset && Data[I + 1] == 0x45) {
      int32_t Value = static_cast<int32_t>(readLE<uint32_t>(Data + I + 3));
      if (Value == -1 || Value == -2)
        Level = Value;
    }
  }
  return Level;
}

/// Follow one `jmp rel32` / `jmp [iat]` veneer and name what it reaches.
/// Returns an empty string when \p Address is not a veneer or its destination
/// has no name.
std::string resolveVeneerTargetName(const BinaryImage &Img, va_t Address) {
  auto Opcode = readScalar<uint8_t>(Img, Address);
  if (!Opcode)
    return {};
  if (*Opcode == 0xE9) {
    auto Displacement = readScalar<int32_t>(Img, Address + 1);
    if (!Displacement)
      return {};
    if (auto Target = addSignedOffset(Address + 5, *Displacement))
      return resolveRoutineName(Img, *Target);
    return {};
  }
  if (*Opcode != 0xFF)
    return {};
  auto Modrm = readScalar<uint8_t>(Img, Address + 1);
  if (!Modrm || *Modrm != 0x25)
    return {};
  auto Slot = readScalar<uint32_t>(Img, Address + 2);
  if (!Slot)
    return {};
  std::string Name = resolveRoutineName(Img, 0, *Slot);
  if (Name.empty())
    if (auto Bound = readScalar<uint32_t>(Img, *Slot))
      Name = resolveRoutineName(Img, *Bound, *Slot);
  return Name;
}

/// What a handler address turned out to be once its name and, failing that,
/// its instruction shape were followed.
struct HandlerIdentity {
  ExceptionPersonality Personality = ExceptionPersonality::Unknown;
  std::string Name;
  /// `FuncInfo` address recovered from a `__ehhandler$` thunk.
  va_t CxxFuncInfoVA = 0;
};

/// Decode the `__ehhandler$` thunk MSVC emits for each x86-32 C++ frame:
///
///     [nop; nop]
///     mov eax, offset __ehfuncinfo$<mangled>
///     jmp __CxxFrameHandler3
///
/// The thunk is how a per-function `FuncInfo` reaches a shared personality
/// that takes it in `EAX`, so the `mov` immediate is the only place the table
/// address appears.
bool decodeCxxHandlerThunk(const BinaryImage &Img, va_t HandlerVA,
                           HandlerIdentity &Identity) {
  // The `mov`/`jmp` pair is the tail of the thunk, not necessarily its first
  // instruction: a /GS build prefixes a cookie check, and an /hotpatch or /Gy
  // build prefixes padding.  Scanning for the pair, and requiring the `jmp` to
  // sit immediately after the `mov`, keeps both variants readable without
  // accepting an unrelated `mov eax, imm32` somewhere in a real function.
  constexpr size_t MaxThunkBytes = 64;
  for (size_t Offset = 0; Offset < MaxThunkBytes; ++Offset) {
    const va_t Cursor = HandlerVA + Offset;
    auto Opcode = readScalar<uint8_t>(Img, Cursor);
    if (!Opcode)
      return false;
    if (*Opcode != 0xB8)
      continue;
    auto FuncInfo = readScalar<uint32_t>(Img, Cursor + 1);
    if (!FuncInfo || *FuncInfo == 0 || !Img.readVA(*FuncInfo, 4))
      continue;

    va_t Target = 0;
    auto Jump = readScalar<uint8_t>(Img, Cursor + 5);
    if (!Jump)
      return false;
    if (*Jump == 0xE9) {
      auto Displacement = readScalar<int32_t>(Img, Cursor + 6);
      if (!Displacement)
        continue;
      auto Resolved = addSignedOffset(Cursor + 10, *Displacement);
      if (!Resolved || !isExecutableAddress(Img, *Resolved))
        continue;
      Target = *Resolved;
    } else if (*Jump == 0xFF) {
      // `jmp dword ptr [__imp___CxxFrameHandler3]`, which is what an import
      // of the personality looks like when the linker did not build a veneer.
      auto Modrm = readScalar<uint8_t>(Img, Cursor + 6);
      if (!Modrm || *Modrm != 0x25)
        continue;
      auto Slot = readScalar<uint32_t>(Img, Cursor + 7);
      if (!Slot)
        continue;
      std::string SlotName = resolveRoutineName(Img, 0, *Slot);
      if (SlotName.empty())
        if (auto Bound = readScalar<uint32_t>(Img, *Slot))
          SlotName = resolveRoutineName(Img, *Bound, *Slot);
      Identity.CxxFuncInfoVA = *FuncInfo;
      ExceptionPersonality Resolved = classifyPersonalityName(SlotName);
      Identity.Personality = isCxxPersonality(Resolved)
                                 ? Resolved
                                 : ExceptionPersonality::CxxFrameHandlerX86;
      Identity.Name = std::move(SlotName);
      return true;
    } else {
      continue;
    }

    Identity.CxxFuncInfoVA = *FuncInfo;
    std::string TargetName = resolveRoutineName(Img, Target);
    if (TargetName.empty())
      TargetName = resolveVeneerTargetName(Img, Target);
    ExceptionPersonality Resolved = classifyPersonalityName(TargetName);
    if (isCxxPersonality(Resolved)) {
      Identity.Personality = Resolved;
      Identity.Name = TargetName;
    } else {
      // A statically linked personality can be stripped of its name.  The
      // thunk shape plus a `FuncInfo` that validates is still proof of the C++
      // model, so record the x86 spelling and let the table decode confirm it.
      Identity.Personality = ExceptionPersonality::CxxFrameHandlerX86;
      Identity.Name = TargetName;
    }
    return true;
  }
  return false;
}

/// Recognize the per-image `_except_handler4` wrapper.
///
/// `_except_handler4` cannot be imported: it has to read *this* image's
/// `__security_cookie` in order to unmask the scope table, so the CRT supplies
/// only the cookie-agnostic `_except_handler4_common` and the compiler emits a
/// local wrapper that supplies the cookie and forwards to it:
///
///     push  ebp / mov ebp, esp / push esi
///     mov   esi, [ebp+8]
///     push  [esi]  /  call  <decode>  /  mov [esi], eax
///     push  [ebp+14h] ... push esi
///     push  offset <cookie check thunk>
///     push  offset __security_cookie
///     call  _except_handler4_common
///
/// A GS-enabled image therefore installs an address that resolves to no known
/// name, which is why a name lookup alone reports the frame as having an
/// unknown personality.  The wrapper is identified here by what it forwards
/// to, corroborated by its use of the cookie the load configuration names —
/// both facts the image states about itself rather than shapes guessed from
/// bytes.
bool isExceptHandler4Wrapper(const BinaryImage &Img, va_t HandlerVA) {
  // Longest wrapper MSVC emits is well inside this; scanning further would
  // start reading whatever routine follows it.
  constexpr size_t MaxWrapperBytes = 96;
  const va_t CookieVA = Img.DynInfo.SecurityCookieRVA == 0
                            ? 0
                            : Img.Base + Img.DynInfo.SecurityCookieRVA;
  bool ForwardsToCommon = false;
  bool UsesSecurityCookie = false;

  for (size_t Offset = 0; Offset < MaxWrapperBytes; ++Offset) {
    const va_t VA = HandlerVA + Offset;
    auto Opcode = readScalar<uint8_t>(Img, VA);
    if (!Opcode)
      break;
    // `push offset __security_cookie`
    if (*Opcode == 0x68 && CookieVA != 0) {
      if (auto Immediate = readScalar<uint32_t>(Img, VA + 1))
        UsesSecurityCookie |= *Immediate == CookieVA;
      continue;
    }
    std::string TargetName;
    if (*Opcode == 0xE8 || *Opcode == 0xE9) {
      auto Displacement = readScalar<int32_t>(Img, VA + 1);
      if (!Displacement)
        continue;
      auto Target = addSignedOffset(VA + 5, *Displacement);
      if (!Target || !isExecutableAddress(Img, *Target))
        continue;
      TargetName = resolveRoutineName(Img, *Target);
      if (TargetName.empty())
        TargetName = resolveVeneerTargetName(Img, *Target);
    } else if (*Opcode == 0xFF) {
      auto Modrm = readScalar<uint8_t>(Img, VA + 1);
      // `call [mem32]` (reg=2) and `jmp [mem32]` (reg=4), absolute form.
      if (!Modrm || (*Modrm != 0x15 && *Modrm != 0x25))
        continue;
      if (auto Slot = readScalar<uint32_t>(Img, VA + 2)) {
        TargetName = resolveRoutineName(Img, 0, *Slot);
        if (TargetName.empty())
          if (auto Bound = readScalar<uint32_t>(Img, *Slot))
            TargetName = resolveRoutineName(Img, *Bound, *Slot);
      }
    } else {
      continue;
    }
    if (classifyPersonalityName(TargetName) ==
        ExceptionPersonality::ExceptHandler4)
      ForwardsToCommon = true;
  }
  return ForwardsToCommon && (UsesSecurityCookie || CookieVA == 0);
}

/// Name a handler, following the `jmp [iat]` and `jmp rel32` veneers a linker
/// may interpose between the pushed address and the routine itself.
HandlerIdentity identifyHandler(const BinaryImage &Img, va_t HandlerVA) {
  HandlerIdentity Identity;
  va_t Cursor = HandlerVA;
  std::vector<va_t> Seen;
  for (unsigned Hop = 0; Hop < MaxHandlerThunkHops; ++Hop) {
    if (std::find(Seen.begin(), Seen.end(), Cursor) != Seen.end())
      break;
    Seen.push_back(Cursor);

    std::string Name = resolveRoutineName(Img, Cursor);
    ExceptionPersonality Resolved = classifyPersonalityName(Name);
    if (Resolved != ExceptionPersonality::Unknown &&
        Resolved != ExceptionPersonality::None) {
      Identity.Personality = Resolved;
      Identity.Name = std::move(Name);
      return Identity;
    }
    if (Identity.Name.empty())
      Identity.Name = Name;

    auto Opcode = readScalar<uint8_t>(Img, Cursor);
    if (!Opcode)
      break;
    if (*Opcode == 0xE9) {
      auto Displacement = readScalar<int32_t>(Img, Cursor + 1);
      if (!Displacement)
        break;
      auto Target = addSignedOffset(Cursor + 5, *Displacement);
      if (!Target)
        break;
      Cursor = *Target;
      continue;
    }
    if (*Opcode == 0xFF) {
      auto Modrm = readScalar<uint8_t>(Img, Cursor + 1);
      if (!Modrm || *Modrm != 0x25)
        break;
      auto Slot = readScalar<uint32_t>(Img, Cursor + 2);
      if (!Slot)
        break;
      std::string SlotName = resolveRoutineName(Img, 0, *Slot);
      if (SlotName.empty())
        if (auto Bound = readScalar<uint32_t>(Img, *Slot))
          SlotName = resolveRoutineName(Img, *Bound, *Slot);
      ExceptionPersonality SlotPersonality = classifyPersonalityName(SlotName);
      if (SlotPersonality != ExceptionPersonality::Unknown &&
          SlotPersonality != ExceptionPersonality::None) {
        Identity.Personality = SlotPersonality;
        Identity.Name = std::move(SlotName);
      }
      break;
    }
    break;
  }
  return Identity;
}

/// True when a scope-table entry names a level that already exists.  Levels
/// are indices into the same table, so a forward or out-of-range reference
/// would make the nesting graph cyclic or dangling.
bool isValidEnclosingLevel(int32_t Level, uint32_t Index, bool IsEH4) {
  if (Level == -1)
    return true;
  if (IsEH4 && Level == -2)
    return true;
  return Level >= 0 && static_cast<uint32_t>(Level) < Index;
}

/// Decode the scope-table entry array.
///
/// The array is unsized: nothing in the image records how many entries a table
/// has, because the runtime only ever indexes it by the try level held in the
/// frame.  Validating entries until one fails is therefore not enough on its
/// own — the compiler emits these tables back to back, so the first entry of
/// the *next* function's table is a perfectly well-formed entry and a walk
/// that only checks well-formedness runs straight into it, attributing another
/// function's handlers to this one.
///
/// \p Limit is the address the next table begins at, which caps the walk at
/// the one boundary the image does establish.  It is zero for the last table
/// in the image, where validation is all there is.
uint32_t decodeScopeRecords(const BinaryImage &Img, va_t ArrayVA, va_t Limit,
                            bool IsEH4,
                            std::vector<RegistrationScopeRecord> &Scopes) {
  for (uint32_t Index = 0; Index < MaxRegistrationRecords; ++Index) {
    uint64_t Offset = uint64_t(Index) * 12;
    if (Offset > InvalidVA - ArrayVA)
      break;
    if (Limit != 0 && ArrayVA + Offset + 12 > Limit)
      break;
    const uint8_t *Entry = Img.readVA(ArrayVA + Offset, 12);
    if (!Entry)
      break;
    int32_t Level = readLE<int32_t>(Entry);
    uint32_t Filter = readLE<uint32_t>(Entry + 4);
    uint32_t Handler = readLE<uint32_t>(Entry + 8);
    if (!isValidEnclosingLevel(Level, Index, IsEH4))
      break;
    // A `__finally` has no filter; an `__except` has both.  An entry with no
    // handler at all describes nothing and marks the end of the array.
    if (Handler == 0 || !isExecutableAddress(Img, Handler))
      break;
    if (Filter != 0 && !isExecutableAddress(Img, Filter))
      break;

    RegistrationScopeRecord Scope;
    Scope.EnclosingLevel = Level;
    Scope.FilterVA = Filter;
    Scope.HandlerVA = Handler;
    Scope.IsFinally = Filter == 0;
    Scopes.push_back(Scope);
  }
  return static_cast<uint32_t>(Scopes.size());
}

/// One `mov dword ptr [ebp+disp], imm32`, the only shape the compiler uses to
/// set the current try level.
struct FrameSlotStore {
  va_t StoreVA = 0;
  va_t EndVA = 0;
  int32_t Displacement = 0;
  int32_t Value = 0;
};

/// Every such store inside a code range.
///
/// This is a byte scan rather than a decode, so it can also match bytes that
/// are the tail of some other instruction.  Nothing downstream trusts a hit on
/// its own: a slot is only believed once the whole set of stores into it reads
/// as a try-level sequence, which arbitrary bytes do not.
std::vector<FrameSlotStore>
findFrameSlotStores(const BinaryImage &Img,
                    const ExceptionAddressRange &Range) {
  std::vector<FrameSlotStore> Stores;
  const Segment *Seg = Img.getSegmentFor(Range.Begin);
  if (!Seg || !Seg->isExecutable() || Range.Begin < Seg->VA ||
      Range.End <= Range.Begin)
    return Stores;
  const uint64_t Begin = Range.Begin - Seg->VA;
  const uint64_t End =
      std::min<uint64_t>(Range.End - Seg->VA, Seg->Data.size());
  if (Begin >= End)
    return Stores;

  const uint8_t *Data = Seg->Data.data();
  for (uint64_t I = Begin; I + 7 <= End; ++I) {
    if (Data[I] != 0xC7)
      continue;
    // ModRM /0 with a base of EBP: mod=01 is the byte displacement and mod=10
    // the dword one.  Both take a trailing imm32.
    if (Data[I + 1] == 0x45) {
      Stores.push_back(
          {static_cast<va_t>(Seg->VA + I), static_cast<va_t>(Seg->VA + I + 7),
           static_cast<int8_t>(Data[I + 2]),
           static_cast<int32_t>(readLE<uint32_t>(Data + I + 3))});
    } else if (Data[I + 1] == 0x85 && I + 10 <= End) {
      Stores.push_back(
          {static_cast<va_t>(Seg->VA + I), static_cast<va_t>(Seg->VA + I + 10),
           static_cast<int32_t>(readLE<uint32_t>(Data + I + 2)),
           static_cast<int32_t>(readLE<uint32_t>(Data + I + 6))});
    }
  }
  return Stores;
}

/// Prove which frame slot holds the current try level, and keep the stores
/// into it.
///
/// The scope table is indexed by a level the runtime reads out of the frame,
/// so the table alone never says which code each scope guards — only the
/// stores do.  Which slot holds the level is not recorded anywhere either, so
/// it has to be proven, and the table itself supplies the vocabulary to prove
/// it with: a try-level slot only ever receives the seed the prologue pushed
/// or the index of a scope the table declares.  A frame slot qualifies when
/// every store into it is one of those values, the seed is among them, and so
/// is at least one real scope index.  Ordinary locals fail on the first count
/// by holding something outside the range and on the second by never being set
/// to the seed.  When more than one slot survives, nothing was proven and no
/// ranges are published.
void recoverTryLevelStores(const BinaryImage &Img,
                           const ExceptionAddressRange &Range, int32_t Seed,
                           size_t ScopeCount, RegistrationChainInfo &Chain) {
  if (ScopeCount == 0 || ScopeCount > MaxRegistrationRecords)
    return;
  const int32_t Highest = static_cast<int32_t>(ScopeCount) - 1;

  std::map<int32_t, std::vector<FrameSlotStore>> BySlot;
  for (const FrameSlotStore &Store : findFrameSlotStores(Img, Range)) {
    // The try level lives in the frame the prologue established, which is
    // below the frame pointer.  A positive displacement addresses an incoming
    // argument and cannot be it.
    if (Store.Displacement < 0)
      BySlot[Store.Displacement].push_back(Store);
  }

  const std::vector<FrameSlotStore> *Winner = nullptr;
  int32_t WinningSlot = 0;
  for (const auto &[Slot, Stores] : BySlot) {
    bool SawSeed = false;
    bool SawScope = false;
    bool AllInRange = true;
    for (const FrameSlotStore &Store : Stores) {
      if (Store.Value == Seed)
        SawSeed = true;
      else if (Store.Value >= 0 && Store.Value <= Highest)
        SawScope = true;
      else
        AllInRange = false;
    }
    if (!AllInRange || !SawSeed || !SawScope)
      continue;
    if (Winner)
      return;
    Winner = &Stores;
    WinningSlot = Slot;
  }
  if (!Winner)
    return;

  Chain.TryLevelOffset = WinningSlot;
  Chain.TryLevelStores.reserve(Winner->size());
  for (const FrameSlotStore &Store : *Winner)
    Chain.TryLevelStores.push_back({Store.StoreVA, Store.EndVA, Store.Value});
  std::sort(Chain.TryLevelStores.begin(), Chain.TryLevelStores.end(),
            [](const RegistrationTryLevelStore &A,
               const RegistrationTryLevelStore &B) {
              return A.StoreVA < B.StoreVA;
            });
}

/// `_except_handler4` prefixes the entry array with the frame displacements of
/// the security cookies it verifies before trusting the table.  A `-2` cookie
/// offset is the sentinel for "this frame has no cookie of that kind".
bool decodeEH4Header(const BinaryImage &Img, va_t TableVA,
                     RegistrationChainInfo &Chain) {
  const uint8_t *Header = Img.readVA(TableVA, 16);
  if (!Header)
    return false;
  Chain.GSCookieOffset = readLE<int32_t>(Header);
  Chain.GSCookieXOROffset = readLE<int32_t>(Header + 4);
  Chain.EHCookieOffset = readLE<int32_t>(Header + 8);
  Chain.EHCookieXOROffset = readLE<int32_t>(Header + 12);
  Chain.HasSecurityCookies = Chain.GSCookieOffset != -2;
  return true;
}

/// Decode the x86-32 `FuncInfo` reached through an `__ehhandler$` thunk.
///
/// The field order matches the x64 form, but three things differ.  Every
/// pointer is an absolute virtual address rather than an image-relative
/// offset.  There is no unwind-help displacement -- that field exists only for
/// the relative-offset targets -- so every field after the IP map count sits
/// four bytes earlier than it does on x64.  And the handler array element is
/// four bytes shorter, because x86 has no parent-frame displacement.
///
/// There is also no IP-to-state map: the current state lives in the
/// registration record the prologue pushed, so the compiler updates it with a
/// store instead of describing it in a table.
bool decodeX86FuncInfo(ExceptionFunction &F, const BinaryImage &Img,
                       va_t FuncInfoVA) {
  // `magicNumber` occupies 29 bits and shares its word with `bbtFlags`, and
  // the magic fixes the length of the record: the original form ends after the
  // IP map pointer, the second adds the exception-specification list, and the
  // third adds `EHFlags`.  Demanding the longest layout would both reject a
  // legacy record near the end of a section and read trailing fields out of
  // whatever data follows it.
  const uint8_t *MagicField = Img.readVA(FuncInfoVA, sizeof(uint32_t));
  if (!MagicField) {
    diagnose(F, ExceptionParseStatus::Malformed, "truncated x86 C++ FuncInfo");
    return false;
  }
  const uint32_t MagicWord = readLE<uint32_t>(MagicField);
  const uint32_t Magic = MagicWord & 0x1FFFFFFFu;
  CxxFuncInfoVersion Version;
  size_t FuncInfoSize;
  switch (Magic) {
  case 0x19930520:
    Version = CxxFuncInfoVersion::Original;
    FuncInfoSize = 0x1c;
    break;
  case 0x19930521:
    Version = CxxFuncInfoVersion::WithExceptionSpecs;
    FuncInfoSize = 0x20;
    break;
  case 0x19930522:
    Version = CxxFuncInfoVersion::WithEHFlags;
    FuncInfoSize = 0x24;
    break;
  default:
    diagnose(F, ExceptionParseStatus::Malformed,
             "unknown x86 C++ FuncInfo magic 0x" + llvm::utohexstr(Magic));
    return false;
  }

  const uint8_t *FI = Img.readVA(FuncInfoVA, FuncInfoSize);
  if (!FI) {
    diagnose(F, ExceptionParseStatus::Malformed, "truncated x86 C++ FuncInfo");
    return false;
  }

  CxxExceptionInfo Info;
  Info.NativeEncoding = CxxExceptionInfo::Encoding::FH3;
  Info.Magic = Magic;
  Info.Version = Version;
  Info.BBTFlags = MagicWord >> 29;

  int32_t MaxState = readLE<int32_t>(FI + 4);
  uint32_t UnwindMapVA = readLE<uint32_t>(FI + 8);
  uint32_t TryCount = readLE<uint32_t>(FI + 12);
  uint32_t TryMapVA = readLE<uint32_t>(FI + 16);
  uint32_t IPCount = readLE<uint32_t>(FI + 20);
  if (Version >= CxxFuncInfoVersion::WithExceptionSpecs)
    Info.ESTypeListVA = readLE<uint32_t>(FI + 28);
  if (Version >= CxxFuncInfoVersion::WithEHFlags) {
    Info.Flags = readLE<uint32_t>(FI + 32);
    Info.IsSynchronous = (Info.Flags & 1u) != 0;
    Info.HasDynamicStackAlignment = (Info.Flags & 2u) != 0;
    Info.IsNoExcept = (Info.Flags & 4u) != 0;
  }
  if (MaxState < 0 ||
      static_cast<uint32_t>(MaxState) > MaxRegistrationRecords ||
      TryCount > MaxRegistrationRecords) {
    diagnose(F, ExceptionParseStatus::Malformed,
             "x86 C++ FuncInfo count exceeds decode budget");
    return false;
  }
  Info.MaxState = static_cast<uint32_t>(MaxState);
  // x86 tracks the current state in the frame, not in a table.  A non-empty
  // IP map here means the record is not the x86 form this decoder proved.
  if (IPCount != 0) {
    diagnose(F, ExceptionParseStatus::Malformed,
             "x86 C++ FuncInfo declares an IP-to-state map");
    return false;
  }

  if (Info.MaxState != 0) {
    uint64_t Bytes = uint64_t(Info.MaxState) * 8;
    const uint8_t *Map = Bytes <= std::numeric_limits<size_t>::max()
                             ? Img.readVA(UnwindMapVA, size_t(Bytes))
                             : nullptr;
    if (!Map) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "truncated x86 C++ unwind map");
      return false;
    }
    Info.UnwindMap.reserve(Info.MaxState);
    for (uint32_t I = 0; I < Info.MaxState; ++I) {
      const uint8_t *E = Map + uint64_t(I) * 8;
      CxxUnwindAction Action;
      Action.ToState = readLE<int32_t>(E);
      Action.ActionVA = readLE<uint32_t>(E + 4);
      if (Action.ActionVA == 0)
        Action.Kind = CxxUnwindAction::ActionKind::None;
      else if (!isExecutableAddress(Img, Action.ActionVA)) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "x86 C++ unwind action is not executable");
        return false;
      }
      Info.UnwindMap.push_back(Action);
    }
  }

  if (TryCount != 0) {
    uint64_t Bytes = uint64_t(TryCount) * 20;
    const uint8_t *Map = Bytes <= std::numeric_limits<size_t>::max()
                             ? Img.readVA(TryMapVA, size_t(Bytes))
                             : nullptr;
    if (!Map) {
      diagnose(F, ExceptionParseStatus::Malformed, "truncated x86 C++ try map");
      return false;
    }
    Info.TryBlocks.reserve(TryCount);
    for (uint32_t I = 0; I < TryCount; ++I) {
      const uint8_t *E = Map + uint64_t(I) * 20;
      CxxTryBlock Try;
      Try.TryLow = readLE<int32_t>(E);
      Try.TryHigh = readLE<int32_t>(E + 4);
      Try.CatchHigh = readLE<int32_t>(E + 8);
      uint32_t CatchCount = readLE<uint32_t>(E + 12);
      uint32_t HandlerArrayVA = readLE<uint32_t>(E + 16);
      if (CatchCount > MaxRegistrationRecords) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "x86 C++ catch count exceeds decode budget");
        return false;
      }
      uint64_t HandlerBytes = uint64_t(CatchCount) * 16;
      const uint8_t *Handlers =
          CatchCount == 0 ? nullptr
                          : Img.readVA(HandlerArrayVA, size_t(HandlerBytes));
      if (CatchCount != 0 && !Handlers) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "truncated x86 C++ handler map");
        return false;
      }
      Try.Handlers.reserve(CatchCount);
      for (uint32_t J = 0; J < CatchCount; ++J) {
        const uint8_t *H = Handlers + uint64_t(J) * 16;
        CxxCatchHandler Catch;
        Catch.Adjectives = readLE<uint32_t>(H);
        Catch.TypeDescriptorVA = readLE<uint32_t>(H + 4);
        Catch.CatchObjectOffset = readLE<int32_t>(H + 8);
        Catch.HandlerVA = readLE<uint32_t>(H + 12);
        if (Catch.TypeDescriptorVA != 0 &&
            !Img.readVA(Catch.TypeDescriptorVA, 1)) {
          diagnose(F, ExceptionParseStatus::Malformed,
                   "x86 C++ type descriptor is not mapped");
          return false;
        }
        if (Catch.HandlerVA == 0 ||
            !isExecutableAddress(Img, Catch.HandlerVA)) {
          diagnose(F, ExceptionParseStatus::Malformed,
                   "x86 C++ catch handler is not executable code");
          return false;
        }
        Try.Handlers.push_back(std::move(Catch));
      }
      Info.TryBlocks.push_back(std::move(Try));
    }
  }

  // The exception-specification list, in the same absolute-pointer spelling
  // the rest of the x86 record uses.  Its elements are `HandlerType` records,
  // so they are the shorter x86 form here too.
  if (Info.ESTypeListVA != 0) {
    const uint8_t *List = Img.readVA(Info.ESTypeListVA, 8);
    if (!List) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "truncated x86 C++ ESTypeList");
      return false;
    }
    int32_t SpecCount = readLE<int32_t>(List);
    uint32_t SpecArrayVA = readLE<uint32_t>(List + 4);
    if (SpecCount < 0 ||
        static_cast<uint32_t>(SpecCount) > MaxRegistrationRecords) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "x86 C++ ESTypeList count exceeds decode budget");
      return false;
    }
    if (SpecCount != 0) {
      const uint8_t *Specs =
          Img.readVA(SpecArrayVA, static_cast<size_t>(SpecCount) * 16);
      if (!Specs) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "truncated x86 C++ ESTypeList type array");
        return false;
      }
      Info.ExceptionSpecTypes.reserve(static_cast<size_t>(SpecCount));
      for (int32_t I = 0; I < SpecCount; ++I) {
        const uint8_t *S = Specs + uint64_t(I) * 16;
        CxxExceptionSpecType Spec;
        Spec.Adjectives = readLE<uint32_t>(S);
        Spec.TypeDescriptorVA = readLE<uint32_t>(S + 4);
        if (Spec.TypeDescriptorVA != 0 &&
            !Img.readVA(Spec.TypeDescriptorVA, 1)) {
          diagnose(F, ExceptionParseStatus::Malformed,
                   "x86 C++ ESTypeList type descriptor is not mapped");
          return false;
        }
        Info.ExceptionSpecTypes.push_back(Spec);
      }
    }
  }

  if (!Info.hasValidStateGraph()) {
    diagnose(F, ExceptionParseStatus::Malformed,
             "invalid x86 C++ exception state graph");
    return false;
  }
  F.Cxx = std::move(Info);
  return true;
}

/// One proven chain-head read and the operands the surrounding prologue
/// materialized for it.
struct InstallSite {
  va_t InstallVA = 0;
  va_t HandlerVA = 0;
  va_t TableVA = 0;
  std::optional<int32_t> TryLevel;
  HandlerIdentity Identity;
  ExceptionAddressRange Range;
};

/// Locate every prologue that links a registration record onto the `FS:[0]`
/// chain, and resolve the handler and table each one installs.
std::vector<InstallSite> findInstallSites(const BinaryImage &Img,
                                          const FunctionRangeMap &Functions,
                                          const SafeSEHTable &SafeSEH) {
  std::vector<InstallSite> Sites;
  std::vector<va_t> CoveredFunctions;

  for (const Segment &Seg : Img.Segments) {
    if (!Seg.isExecutable() || Seg.Data.size() < 8)
      continue;
    const uint8_t *Data = Seg.Data.data();
    const size_t Size = Seg.Data.size();
    for (size_t I = 0; I < Size; ++I) {
      if (Data[I] != 0x64)
        continue;
      if (chainHeadReadLength(Data + I, Size - I) == 0)
        continue;

      InstallSite Site;
      Site.InstallVA = Seg.VA + I;
      Site.TryLevel = findSeededTryLevel(Seg, I);

      // Choose the handler among the constants the prologue produced.  When
      // the image published a SafeSEH table the loader will only ever dispatch
      // to an address in it, which makes membership proof rather than
      // heuristic; otherwise the constant has to name code that classifies as
      // a handler on its own.
      std::optional<ExceptionAddressRange> Range =
          Functions.find(Img, Site.InstallVA);
      if (!Range)
        continue;

      std::vector<InstallOperand> Operands = collectInstallOperands(Seg, I);
      for (const InstallOperand &Candidate : Operands) {
        if (Candidate.Value == 0 || !isExecutableAddress(Img, Candidate.Value))
          continue;
        // A handler inside the installing function is a Delphi `TExcDesc`,
        // which the Delphi decoder has already claimed.  Every Windows
        // dialect installs a routine the whole image shares, so none of them
        // can produce a handler here.
        if (Range->contains(Candidate.Value))
          continue;
        if (SafeSEH.isPresent() && !SafeSEH.contains(Candidate.Value))
          continue;
        HandlerIdentity Probe = identifyHandler(Img, Candidate.Value);
        if (Probe.Personality == ExceptionPersonality::Unknown ||
            Probe.Personality == ExceptionPersonality::None) {
          if (!decodeCxxHandlerThunk(Img, Candidate.Value, Probe) &&
              isExceptHandler4Wrapper(Img, Candidate.Value)) {
            Probe.Personality = ExceptionPersonality::ExceptHandler4;
            Probe.Name = getExceptionPersonalityName(
                ExceptionPersonality::ExceptHandler4);
          }
        }
        const bool Classified =
            Probe.Personality != ExceptionPersonality::Unknown &&
            Probe.Personality != ExceptionPersonality::None;
        if (!Classified && !SafeSEH.isPresent())
          continue;
        Site.HandlerVA = Candidate.Value;
        Site.Identity = Probe;
        if (Classified)
          break;
      }
      if (Site.HandlerVA == 0)
        continue;

      // The scope table is the remaining constant that decodes as one.  A C++
      // frame has none here because its table travels inside the handler.
      if (Site.Identity.CxxFuncInfoVA == 0)
        for (const InstallOperand &Candidate : Operands) {
          if (Candidate.Value == Site.HandlerVA || Candidate.Value == 0)
            continue;
          if (isExecutableAddress(Img, Candidate.Value) ||
              !Img.readVA(Candidate.Value, 12))
            continue;
          Site.TableVA = Candidate.Value;
        }

      // A function installs one record; a second hit inside the same body is
      // the same frame being re-established, not a new one.
      if (std::find(CoveredFunctions.begin(), CoveredFunctions.end(),
                    Range->Begin) != CoveredFunctions.end())
        continue;
      CoveredFunctions.push_back(Range->Begin);
      Site.Range = *Range;
      Sites.push_back(std::move(Site));
    }
  }
  return Sites;
}

/// Resolve the frames served by a shared prologue helper.
///
/// At `/O1` and `/O2` MSVC stops inlining the SEH prologue and calls
/// `__SEH_prolog4` instead, a routine that pushes the handler, links the
/// record, and then masks with `__security_cookie` a scope table its *caller*
/// supplied:
///
///     push  <frame size>
///     push  offset <scope table>
///     call  __SEH_prolog4
///
/// The chain-head read therefore sits in the helper, where the table never
/// appears at all.  Attributing the record to the helper would describe one
/// frame the program does not have and lose every frame it does, so a site
/// that proved a handler but no table is resolved by finding the calls.
///
/// Returns the sites the callers establish, or nothing when \p Helper is not a
/// helper — an ordinary frame whose table simply could not be read has no
/// callers that push one.
std::vector<InstallSite>
resolvePrologueHelperCallers(const BinaryImage &Img,
                             const FunctionRangeMap &Functions,
                             const InstallSite &Helper) {
  std::vector<InstallSite> Callers;
  std::vector<va_t> Covered;

  for (const Segment &Seg : Img.Segments) {
    if (!Seg.isExecutable() || Seg.Data.size() < 5)
      continue;
    const uint8_t *Data = Seg.Data.data();
    const size_t Size = Seg.Data.size();
    for (size_t I = 0; I + 5 <= Size; ++I) {
      if (Data[I] != 0xE8)
        continue;
      const va_t CallVA = Seg.VA + I;
      // The helper's own entry usually carries no symbol, so the range the
      // install was attributed to starts at whatever function precedes it.
      // What is certain is that a call establishing this frame has to land in
      // the code that runs before the link and close enough to it to be the
      // same prologue -- a call further back would be some other routine that
      // happens to share the range.
      std::optional<va_t> Target =
          addSignedOffset(CallVA + 5, readLE<int32_t>(Data + I + 1));
      if (!Target || *Target > Helper.InstallVA ||
          Helper.InstallVA - *Target > MaxInstallWindow)
        continue;
      if (!Helper.Range.contains(*Target))
        continue;

      std::optional<ExceptionAddressRange> Range = Functions.find(Img, CallVA);
      if (!Range || Range->overlaps(Helper.Range))
        continue;
      if (std::find(Covered.begin(), Covered.end(), Range->Begin) !=
          Covered.end())
        continue;

      InstallSite Site;
      Site.InstallVA = CallVA;
      Site.HandlerVA = Helper.HandlerVA;
      Site.Identity = Helper.Identity;
      Site.TryLevel = Helper.TryLevel;
      Site.Range = *Range;
      for (const InstallOperand &Candidate : collectInstallOperands(Seg, I)) {
        if (Candidate.Value == 0 || Candidate.Value == Helper.HandlerVA)
          continue;
        if (isExecutableAddress(Img, Candidate.Value) ||
            !Img.readVA(Candidate.Value, 12))
          continue;
        Site.TableVA = Candidate.Value;
      }
      if (Site.TableVA == 0)
        continue;

      Covered.push_back(Range->Begin);
      Callers.push_back(std::move(Site));
    }
  }
  return Callers;
}

/// Replace every shared-prologue-helper site with the frames it serves.
void expandPrologueHelpers(const BinaryImage &Img,
                           const FunctionRangeMap &Functions,
                           std::vector<InstallSite> &Sites) {
  std::vector<InstallSite> Expanded;
  for (InstallSite &Site : Sites) {
    const bool LooksLikeHelper = Site.TableVA == 0 && Site.HandlerVA != 0 &&
                                 Site.Identity.CxxFuncInfoVA == 0;
    std::vector<InstallSite> Callers;
    if (LooksLikeHelper)
      Callers = resolvePrologueHelperCallers(Img, Functions, Site);
    if (Callers.empty()) {
      Expanded.push_back(std::move(Site));
      continue;
    }
    for (InstallSite &Caller : Callers)
      Expanded.push_back(std::move(Caller));
  }
  Sites = std::move(Expanded);
}

/// Address at which the table after \p TableVA begins, or zero when it is the
/// last one.  \p Sorted holds every table address the image was proven to
/// install, which is what bounds an otherwise unsized entry array.
va_t findNextTableAddress(const std::vector<va_t> &Sorted, va_t TableVA) {
  auto It = std::upper_bound(Sorted.begin(), Sorted.end(), TableVA);
  return It == Sorted.end() ? 0 : *It;
}

} // namespace

void parseX86RegistrationExceptions(BinaryImage &Img) {
  if (Img.Arch != Arch::X86 || Img.Format != BinaryFormat::COFF)
    return;

  const FunctionRangeMap Functions(Img);
  const SafeSEHTable SafeSEH(Img);
  std::vector<InstallSite> Sites = findInstallSites(Img, Functions, SafeSEH);
  expandPrologueHelpers(Img, Functions, Sites);
  if (Sites.empty())
    return;

  // Every table address the image installs, so each entry array can be capped
  // at the next one.  Both the scope tables and the C++ `FuncInfo` records
  // take part: the compiler emits them into the same read-only region, so a
  // `FuncInfo` is just as much a boundary for the scope table before it.
  std::vector<va_t> TableAddresses;
  for (const InstallSite &Site : Sites) {
    if (Site.Identity.CxxFuncInfoVA != 0)
      TableAddresses.push_back(Site.Identity.CxxFuncInfoVA);
    if (Site.TableVA != 0)
      TableAddresses.push_back(Site.TableVA);
  }
  std::sort(TableAddresses.begin(), TableAddresses.end());
  TableAddresses.erase(
      std::unique(TableAddresses.begin(), TableAddresses.end()),
      TableAddresses.end());

  std::vector<ExceptionFunction> Recovered;
  Recovered.reserve(Sites.size());
  for (const InstallSite &Site : Sites) {
    const HandlerIdentity &Identity = Site.Identity;
    ExceptionFunction F;
    F.CodeRange = Site.Range;
    F.Kind = RuntimeFunctionKind::Primary;
    F.PersonalityVA = Site.HandlerVA;
    F.PersonalityName = Identity.Name;
    F.Personality = Identity.Personality;

    RegistrationChainInfo Chain;
    Chain.HandlerVA = Site.HandlerVA;
    Chain.ChainInstallVA = Site.InstallVA;
    Chain.ScopeTableVA = Site.TableVA;

    if (Identity.CxxFuncInfoVA != 0) {
      F.Encoding = ExceptionEncoding::X86CxxFuncInfo;
      F.HandlerDataVA = Identity.CxxFuncInfoVA;
      Chain.ScopeTableVA = Identity.CxxFuncInfoVA;
      if (F.Personality == ExceptionPersonality::Unknown)
        F.Personality = ExceptionPersonality::CxxFrameHandlerX86;
      decodeX86FuncInfo(F, Img, Identity.CxxFuncInfoVA);
    } else if (Site.TableVA != 0 && Img.readVA(Site.TableVA, 12)) {
      // `_except_handler4` seeds -2 as the initial try level and prefixes
      // its table with cookie displacements; `_except_handler3` seeds -1
      // and starts at the entry array.  When the handler kept its name that
      // is authoritative, otherwise the sentinel decides.
      bool IsEH4 = F.Personality == ExceptionPersonality::ExceptHandler4 ||
                   (F.Personality != ExceptionPersonality::ExceptHandler3 &&
                    Site.TryLevel && *Site.TryLevel == -2);
      F.HandlerDataVA = Site.TableVA;
      va_t ArrayVA = Site.TableVA;
      if (IsEH4) {
        if (!decodeEH4Header(Img, Site.TableVA, Chain)) {
          diagnose(F, ExceptionParseStatus::Malformed,
                   "truncated _except_handler4 scope-table header");
        } else {
          ArrayVA = Site.TableVA + 16;
        }
        F.Encoding = ExceptionEncoding::X86ScopeTableEH4;
        if (F.Personality == ExceptionPersonality::Unknown)
          F.Personality = ExceptionPersonality::ExceptHandler4;
      } else {
        F.Encoding = ExceptionEncoding::X86ScopeTableEH3;
        if (F.Personality == ExceptionPersonality::Unknown)
          F.Personality = ExceptionPersonality::ExceptHandler3;
      }
      // Both sentinels mean "no scope is current"; which one this frame uses
      // follows from the handler it installed.
      Chain.SeededTryLevel = IsEH4 ? -2 : -1;
      const va_t Limit = findNextTableAddress(TableAddresses, Site.TableVA);
      if (decodeScopeRecords(Img, ArrayVA, Limit, IsEH4, Chain.Scopes) == 0)
        diagnose(F, ExceptionParseStatus::Partial,
                 "x86 scope table at 0x" + llvm::utohexstr(ArrayVA) +
                     " declares no usable entry");
    } else {
      F.Encoding = ExceptionEncoding::X86ScopeTableEH3;
      diagnose(F, ExceptionParseStatus::Partial,
               "x86 registration record installs a handler with no "
               "recoverable table");
    }

    if (Chain.SeededTryLevel)
      recoverTryLevelStores(Img, F.CodeRange, *Chain.SeededTryLevel,
                            Chain.Scopes.size(), Chain);

    F.Registration = std::move(Chain);
    if (F.Personality == ExceptionPersonality::Unknown)
      diagnose(F, ExceptionParseStatus::Partial,
               "unknown x86 registration handler");

    Recovered.push_back(std::move(F));
  }

  for (ExceptionFunction &F : Recovered) {
    Img.ExceptionMetadata.ParseStatus = mergeExceptionParseStatus(
        Img.ExceptionMetadata.ParseStatus, F.ParseStatus);
    Img.ExceptionMetadata.addModel(F.model());
    Img.ExceptionMetadata.Functions.push_back(std::move(F));
  }
  Img.ExceptionMetadata.rebuildIndex();
}

} // namespace neverd::coff_loader
