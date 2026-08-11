//===- COFFDelphiEH.cpp - Delphi x86-32 registration frames --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/COFF/COFFDelphiEH.h"

#include "neverd/Support/BinaryEncoding.h"
#include "neverd/loader/LanguageRuntime.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace neverd::coff_loader {
namespace {

/// Distances *back* from a Delphi class reference to the VMT fields, from the
/// `vmt*` constants in `System.pas`.  A class reference points at the virtual
/// method slots and the metadata sits in front of them, so every one of these
/// is subtracted.
constexpr va_t VmtSelfPtrBack = 76;
constexpr va_t VmtClassNameBack = 44;
constexpr va_t VmtInstanceSizeBack = 40;
constexpr va_t VmtParentBack = 36;

/// Bound on one `except on` arm table.  Delphi allows any number of arms, but
/// a count beyond this is a mis-identified descriptor rather than a program.
constexpr uint32_t MaxOnExceptionArms = 256;
/// Longest class name accepted from a VMT.  Delphi's is a `ShortString`, so
/// 255 is the encoding's own limit.
constexpr uint8_t MaxClassNameLength = 255;
/// Largest instance size accepted while validating a VMT.
constexpr uint32_t MaxInstanceSize = 1u << 24;
/// Bound on the number of frames recovered from one image.
constexpr size_t MaxDelphiFrames = 1u << 18;

template <typename T>
std::optional<T> readScalar(const BinaryImage &Img, va_t Address) {
  const uint8_t *P = Img.readVA(Address, sizeof(T));
  if (!P)
    return std::nullopt;
  return readLE<T>(P);
}

bool isExecutableAddress(const BinaryImage &Img, va_t Address) {
  const Segment *Seg = Img.getSegmentFor(Address);
  return Seg && Seg->isExecutable() && Img.readVA(Address, 1) != nullptr;
}

std::optional<va_t> addSignedOffset(va_t Base, int64_t Displacement) {
  if (Displacement >= 0) {
    if (static_cast<uint64_t>(Displacement) > InvalidVA - Base)
      return std::nullopt;
    return Base + static_cast<uint64_t>(Displacement);
  }
  const uint64_t Magnitude = static_cast<uint64_t>(-Displacement);
  if (Magnitude > Base)
    return std::nullopt;
  return Base - Magnitude;
}

/// Target of a `jmp rel32` at \p VA, or nullopt when there is not one.
std::optional<va_t> decodeNearJump(const BinaryImage &Img, va_t VA) {
  auto Opcode = readScalar<uint8_t>(Img, VA);
  if (!Opcode || *Opcode != 0xE9)
    return std::nullopt;
  auto Displacement = readScalar<int32_t>(Img, VA + 1);
  if (!Displacement)
    return std::nullopt;
  return addSignedOffset(VA + 5, *Displacement);
}

//===----------------------------------------------------------------------===//
// Prologue recognition
//===----------------------------------------------------------------------===//

/// A `FS:[0]` access whose displacement is zero, in either the absolute or the
/// zeroed-register form.  Delphi zeroes a register and addresses the chain
/// head through it, which is why matching only the absolute encoding — the one
/// MSVC and clang-cl emit — misses every Delphi frame in an image.
///
/// \p Reg is the ModRM reg field the caller requires: 4 (`ESP`) for the store
/// that publishes the record, 6 for the `push` that saves the old head.
size_t chainAccessLength(const uint8_t *Code, size_t Available, uint8_t Opcode,
                         uint8_t Reg) {
  if (Available < 3 || Code[0] != 0x64 || Code[1] != Opcode)
    return 0;
  const uint8_t Modrm = Code[2];
  if (((Modrm >> 3) & 7) != Reg)
    return 0;
  if ((Modrm & 0xC0) != 0)
    return 0;
  const uint8_t Rm = Modrm & 7;
  // rm=4 selects a SIB byte and rm=5 the absolute displacement form; every
  // other value is a plain register indirection with no displacement.
  if (Rm == 4)
    return 0;
  if (Rm == 5)
    return Available >= 7 && readLE<uint32_t>(Code + 3) == 0 ? 7 : 0;
  return 3;
}

/// One recognized `TExcFrame` install.
struct DelphiInstall {
  va_t InstallVA = 0;
  va_t DescriptorVA = 0;
};

/// Match the prologue sequence that links a Delphi `TExcFrame`:
///
///     push  ebp                       ; TExcFrame.hEBP
///     push  offset <TExcDesc>         ; TExcFrame.desc
///     push  dword ptr fs:[reg]        ; TExcFrame.next
///     mov   dword ptr fs:[reg], esp   ; publish
///
/// The three pushes build the record in the layout the RTL reads, so their
/// order is fixed and matching all of them — rather than the store alone —
/// is what separates a Delphi frame from any other use of the chain.
std::optional<DelphiInstall> matchInstall(const Segment &Seg, size_t Offset) {
  const uint8_t *Data = Seg.Data.data();
  const size_t Size = Seg.Data.size();
  const size_t StoreLength =
      chainAccessLength(Data + Offset, Size - Offset, 0x89, /*ESP=*/4);
  if (StoreLength == 0)
    return std::nullopt;
  // `push dword ptr fs:[...]` must sit immediately in front of the store, in
  // the same addressing form.
  if (Offset < StoreLength)
    return std::nullopt;
  const size_t PushNext = Offset - StoreLength;
  if (chainAccessLength(Data + PushNext, Size - PushNext, 0xFF, /*/6=*/6) !=
      StoreLength)
    return std::nullopt;
  // `push imm32` holding the descriptor.
  if (PushNext < 5 || Data[PushNext - 5] != 0x68)
    return std::nullopt;
  const size_t PushDesc = PushNext - 5;
  // `push ebp`.
  if (PushDesc < 1 || Data[PushDesc - 1] != 0x55)
    return std::nullopt;

  DelphiInstall Install;
  Install.InstallVA = Seg.VA + Offset;
  Install.DescriptorVA = readLE<uint32_t>(Data + PushDesc + 1);
  return Install;
}

//===----------------------------------------------------------------------===//
// Class references
//===----------------------------------------------------------------------===//

/// Read the `ShortString` a Delphi VMT points at for its class name.
std::string readClassName(const BinaryImage &Img, va_t VmtVA) {
  if (VmtVA < VmtSelfPtrBack)
    return {};
  auto NamePtr = readScalar<uint32_t>(Img, VmtVA - VmtClassNameBack);
  if (!NamePtr || *NamePtr == 0)
    return {};
  auto Length = readScalar<uint8_t>(Img, *NamePtr);
  if (!Length || *Length == 0 || *Length > MaxClassNameLength)
    return {};
  const uint8_t *Chars = Img.readVA(*NamePtr + 1, *Length);
  if (!Chars)
    return {};
  std::string Name(reinterpret_cast<const char *>(Chars), *Length);
  // A Pascal identifier; anything else means the pointer was not a name.
  for (char C : Name)
    if (static_cast<unsigned char>(C) < 0x20 ||
        static_cast<unsigned char>(C) > 0x7E)
      return {};
  return Name;
}

/// True when \p VmtVA addresses a Delphi class VMT.
///
/// `vmtSelfPtr` makes this decisive rather than probable: the linker stores
/// the VMT's own address there, so a candidate that points at itself is one,
/// and the odds of unrelated data holding its own address are nil.
bool isClassVMT(const BinaryImage &Img, va_t VmtVA) {
  if (VmtVA < VmtSelfPtrBack)
    return false;
  auto SelfPtr = readScalar<uint32_t>(Img, VmtVA - VmtSelfPtrBack);
  if (!SelfPtr || *SelfPtr != VmtVA)
    return false;
  auto InstanceSize = readScalar<uint32_t>(Img, VmtVA - VmtInstanceSizeBack);
  if (!InstanceSize || *InstanceSize == 0 || *InstanceSize > MaxInstanceSize)
    return false;
  // `vmtParent` is null only for `TObject`; anything else names a VMT slot.
  auto Parent = readScalar<uint32_t>(Img, VmtVA - VmtParentBack);
  if (!Parent)
    return false;
  if (*Parent != 0 && !Img.readVA(*Parent, 4))
    return false;
  return true;
}

/// Resolve one `TExcDescEntry.vTable`, which holds the address of the slot the
/// class reference lives in rather than the reference itself.
DelphiOnExceptionEntry resolveArm(const BinaryImage &Img, uint32_t SlotVA,
                                  uint32_t HandlerVA) {
  DelphiOnExceptionEntry Arm;
  Arm.HandlerVA = HandlerVA;
  Arm.ClassSlotVA = SlotVA;
  if (SlotVA == 0) {
    Arm.IsCatchAll = true;
    return Arm;
  }
  if (auto ClassVA = readScalar<uint32_t>(Img, SlotVA)) {
    Arm.ClassVA = *ClassVA;
    if (*ClassVA != 0 && isClassVMT(Img, *ClassVA))
      Arm.ClassName = readClassName(Img, *ClassVA);
  }
  return Arm;
}

//===----------------------------------------------------------------------===//
// Descriptor payload
//===----------------------------------------------------------------------===//

/// What the bytes after a descriptor's jump were found to be.  The shapes are
/// mutually exclusive in practice, which is what lets the RTL routine each
/// descriptor targets be identified without a symbol.
struct DescriptorShape {
  bool LooksLikeOnException = false;
  bool LooksLikeFinally = false;
  va_t FinallyBodyVA = 0;
  std::vector<DelphiOnExceptionEntry> Arms;
};

/// Read the `cnt`/`excTab` payload of a `@HandleOnException` descriptor.
///
/// The count is not self-describing, so every arm is validated: a handler must
/// be code, and a class slot must either be the null of an `else` arm or hold
/// something that reads as a class.  Requiring the whole table to validate —
/// not merely its first arm — is what keeps a `@HandleAnyException`
/// descriptor, whose handler body begins at exactly this address, from being
/// read as a one-arm table.
bool readOnExceptionTable(const BinaryImage &Img, va_t PayloadVA,
                          const ExceptionAddressRange &Function,
                          std::vector<DelphiOnExceptionEntry> &Arms) {
  auto Count = readScalar<uint32_t>(Img, PayloadVA);
  if (!Count || *Count == 0 || *Count > MaxOnExceptionArms)
    return false;
  std::vector<DelphiOnExceptionEntry> Decoded;
  Decoded.reserve(*Count);
  unsigned NamedClasses = 0;
  for (uint32_t I = 0; I < *Count; ++I) {
    const va_t EntryVA = PayloadVA + 4 + uint64_t(I) * 8;
    auto Slot = readScalar<uint32_t>(Img, EntryVA);
    auto Handler = readScalar<uint32_t>(Img, EntryVA + 4);
    if (!Slot || !Handler)
      return false;
    if (*Handler == 0 || !isExecutableAddress(Img, *Handler))
      return false;
    // Every arm of one `except` statement is compiled into the statement's own
    // function, so an arm pointing elsewhere means the table was invented.
    if (Function.isValid() && !Function.contains(*Handler))
      return false;
    if (*Slot != 0 && !Img.readVA(*Slot, 4))
      return false;
    DelphiOnExceptionEntry Arm = resolveArm(Img, *Slot, *Handler);
    // An arm whose slot holds something that is not a class means this is not
    // an arm table.  An `else` arm carries no class and is exempt.
    if (!Arm.IsCatchAll && Arm.ClassName.empty() &&
        !(Arm.ClassVA != 0 && isClassVMT(Img, Arm.ClassVA)))
      return false;
    NamedClasses += !Arm.IsCatchAll;
    Decoded.push_back(std::move(Arm));
  }
  // A table of nothing but `else` arms is what a run of zeroes looks like.
  if (NamedClasses == 0)
    return false;
  Arms = std::move(Decoded);
  return true;
}

DescriptorShape readDescriptorShape(const BinaryImage &Img, va_t DescriptorVA,
                                    const ExceptionAddressRange &Function) {
  DescriptorShape Shape;
  const va_t PayloadVA = DescriptorVA + 5;

  if (readOnExceptionTable(Img, PayloadVA, Function, Shape.Arms))
    Shape.LooksLikeOnException = true;

  // `jmp <finally body>`: the cleanup lives in the same function, and the
  // descriptor is the only thing that names it.
  if (std::optional<va_t> Body = decodeNearJump(Img, PayloadVA))
    if (isExecutableAddress(Img, *Body) &&
        (!Function.isValid() || Function.contains(*Body))) {
      Shape.LooksLikeFinally = true;
      Shape.FinallyBodyVA = *Body;
    }
  return Shape;
}

//===----------------------------------------------------------------------===//
// Runtime routine identity
//===----------------------------------------------------------------------===//

/// Evidence gathered about one RTL routine from every descriptor that jumps to
/// it.
struct RuntimeRoutine {
  DelphiHandlerKind Kind = DelphiHandlerKind::Unknown;
  std::string Name;
  bool Named = false;
  unsigned OnExceptionVotes = 0;
  unsigned FinallyVotes = 0;
  unsigned Descriptors = 0;
};

DelphiHandlerKind classifyRuntimeName(llvm::StringRef Name) {
  if (Name == "@HandleFinally" || Name == "_HandleFinally")
    return DelphiHandlerKind::Finally;
  if (Name == "@HandleAnyException" || Name == "_HandleAnyException")
    return DelphiHandlerKind::AnyException;
  if (Name == "@HandleOnException" || Name == "_HandleOnException")
    return DelphiHandlerKind::OnException;
  if (Name == "@HandleAutoException" || Name == "_HandleAutoException")
    return DelphiHandlerKind::AutoException;
  return DelphiHandlerKind::Unknown;
}

/// Decide what each RTL routine is from the descriptors that target it.
///
/// A stripped Delphi image names none of these routines, but it does not have
/// to: every descriptor that jumps to one is laid out for that routine to
/// read, so the routine's identity is written across all of its descriptors
/// even though no single one proves it.  A `@HandleAnyException` body can
/// begin with a jump and so look like a `@HandleFinally` descriptor; it cannot
/// do so in every frame that shares the routine, which is why the vote is
/// taken per routine rather than per descriptor.
void classifyRuntimeRoutines(llvm::DenseMap<va_t, RuntimeRoutine> &Routines) {
  for (auto &[Address, Routine] : Routines) {
    if (Routine.Named && Routine.Kind != DelphiHandlerKind::Unknown)
      continue;
    if (Routine.OnExceptionVotes * 2 > Routine.Descriptors)
      Routine.Kind = DelphiHandlerKind::OnException;
    else if (Routine.FinallyVotes * 2 > Routine.Descriptors)
      Routine.Kind = DelphiHandlerKind::Finally;
    else
      Routine.Kind = DelphiHandlerKind::AnyException;
  }
}

/// Code range of the function containing an address, bounded by the next
/// discovered function start.
class FunctionRanges {
public:
  explicit FunctionRanges(const BinaryImage &Img) {
    for (const Symbol &Sym : Img.Symbols)
      if (Sym.IsFunc && isExecutableAddress(Img, Sym.Addr))
        Starts.push_back(Sym.Addr);
    std::sort(Starts.begin(), Starts.end());
    Starts.erase(std::unique(Starts.begin(), Starts.end()), Starts.end());
  }

  ExceptionAddressRange find(const BinaryImage &Img, va_t Address) const {
    auto It = std::upper_bound(Starts.begin(), Starts.end(), Address);
    if (It == Starts.begin())
      return {};
    const va_t Begin = *std::prev(It);
    const Segment *Seg = Img.getSegmentFor(Begin);
    if (!Seg || !Seg->isExecutable())
      return {};
    const uint64_t Usable = std::min<uint64_t>(Seg->Size, Seg->Data.size());
    if (Usable > InvalidVA - Seg->VA)
      return {};
    const va_t SegEnd = Seg->VA + Usable;
    const va_t End = It == Starts.end() ? SegEnd : std::min(*It, SegEnd);
    if (End <= Begin || Address >= End)
      return {};
    return ExceptionAddressRange{Begin, End};
  }

private:
  std::vector<va_t> Starts;
};

/// A frame that passed prologue matching, before its RTL routine is known.
struct PendingFrame {
  DelphiInstall Install;
  ExceptionAddressRange Function;
  va_t RuntimeHandlerVA = 0;
  DescriptorShape Shape;
};

std::vector<PendingFrame> collectFrames(const BinaryImage &Img,
                                        const FunctionRanges &Functions) {
  std::vector<PendingFrame> Frames;
  for (const Segment &Seg : Img.Segments) {
    if (!Seg.isExecutable() || Seg.Data.size() < 16)
      continue;
    const size_t Size = Seg.Data.size();
    for (size_t I = 0; I + 3 <= Size; ++I) {
      if (Seg.Data[I] != 0x64)
        continue;
      std::optional<DelphiInstall> Install = matchInstall(Seg, I);
      if (!Install)
        continue;
      if (Frames.size() >= MaxDelphiFrames)
        return Frames;

      const ExceptionAddressRange Function =
          Functions.find(Img, Install->InstallVA);
      // The descriptor is code the compiler placed in the same function as the
      // frame it belongs to.  A pushed address outside it is a scope table or
      // a shared personality — an SEH frame, not a Delphi one.
      if (!Function.isValid() || !Function.contains(Install->DescriptorVA))
        continue;
      std::optional<va_t> Runtime = decodeNearJump(Img, Install->DescriptorVA);
      if (!Runtime || !isExecutableAddress(Img, *Runtime))
        continue;
      // The RTL routine is shared, so it is never the frame's own function.
      if (Function.contains(*Runtime))
        continue;

      PendingFrame Frame;
      Frame.Install = *Install;
      Frame.Function = Function;
      Frame.RuntimeHandlerVA = *Runtime;
      Frame.Shape = readDescriptorShape(Img, Install->DescriptorVA, Function);
      Frames.push_back(std::move(Frame));
    }
  }
  return Frames;
}

llvm::DenseMap<va_t, RuntimeRoutine>
buildRoutineTable(const BinaryImage &Img,
                  const std::vector<PendingFrame> &Frames) {
  llvm::DenseMap<va_t, RuntimeRoutine> Routines;
  for (const PendingFrame &Frame : Frames) {
    RuntimeRoutine &Routine = Routines[Frame.RuntimeHandlerVA];
    if (Routine.Descriptors == 0) {
      Routine.Name = resolveRoutineName(Img, Frame.RuntimeHandlerVA);
      Routine.Kind = classifyRuntimeName(Routine.Name);
      Routine.Named = Routine.Kind != DelphiHandlerKind::Unknown;
    }
    ++Routine.Descriptors;
    Routine.OnExceptionVotes += Frame.Shape.LooksLikeOnException;
    Routine.FinallyVotes += Frame.Shape.LooksLikeFinally;
  }
  classifyRuntimeRoutines(Routines);
  return Routines;
}

} // namespace

bool hasDelphiRegistrationFrames(const BinaryImage &Img) {
  if (Img.Arch != Arch::X86 || Img.Format != BinaryFormat::COFF)
    return false;
  const FunctionRanges Functions(Img);
  return !collectFrames(Img, Functions).empty();
}

void parseDelphiExceptions(BinaryImage &Img) {
  if (Img.Arch != Arch::X86 || Img.Format != BinaryFormat::COFF)
    return;

  const FunctionRanges Functions(Img);
  const std::vector<PendingFrame> Frames = collectFrames(Img, Functions);
  if (Frames.empty())
    return;
  const llvm::DenseMap<va_t, RuntimeRoutine> Routines =
      buildRoutineTable(Img, Frames);

  ExceptionInfo &Info = Img.ExceptionMetadata;
  size_t Added = 0;
  for (const PendingFrame &Frame : Frames) {
    auto It = Routines.find(Frame.RuntimeHandlerVA);
    if (It == Routines.end())
      continue;
    const RuntimeRoutine &Routine = It->second;

    DelphiFrameInfo Delphi;
    Delphi.DescriptorVA = Frame.Install.DescriptorVA;
    Delphi.ChainInstallVA = Frame.Install.InstallVA;
    Delphi.RuntimeHandlerVA = Frame.RuntimeHandlerVA;
    Delphi.RuntimeHandlerName = Routine.Name;
    Delphi.RuntimeHandlerNamed = Routine.Named;
    Delphi.Kind = Routine.Kind;

    ExceptionFunction F;
    F.CodeRange = Frame.Function;
    F.Kind = RuntimeFunctionKind::Primary;
    F.Encoding = ExceptionEncoding::DelphiX86Chain;
    F.Personality = ExceptionPersonality::DelphiX86Handler;
    F.PersonalityName = Routine.Name;
    F.PersonalityVA = Frame.RuntimeHandlerVA;
    F.HandlerDataVA = Frame.Install.DescriptorVA;

    switch (Routine.Kind) {
    case DelphiHandlerKind::Finally:
      if (Frame.Shape.LooksLikeFinally) {
        Delphi.FinallyBodyVA = Frame.Shape.FinallyBodyVA;
      } else {
        F.ParseStatus = mergeExceptionParseStatus(
            F.ParseStatus, ExceptionParseStatus::Partial);
        F.Diagnostics.push_back("Delphi finally descriptor at 0x" +
                                llvm::utohexstr(Frame.Install.DescriptorVA) +
                                " is not followed by a jump to a cleanup body");
      }
      break;
    case DelphiHandlerKind::OnException:
      if (Frame.Shape.LooksLikeOnException) {
        Delphi.OnExceptions = Frame.Shape.Arms;
      } else {
        F.ParseStatus = mergeExceptionParseStatus(
            F.ParseStatus, ExceptionParseStatus::Partial);
        F.Diagnostics.push_back("Delphi on-exception descriptor at 0x" +
                                llvm::utohexstr(Frame.Install.DescriptorVA) +
                                " carries no readable arm table");
      }
      break;
    case DelphiHandlerKind::AnyException:
    case DelphiHandlerKind::AutoException:
      Delphi.ExceptBodyVA = Frame.Install.DescriptorVA + 5;
      break;
    case DelphiHandlerKind::Unknown:
      F.ParseStatus = mergeExceptionParseStatus(F.ParseStatus,
                                                ExceptionParseStatus::Partial);
      F.Diagnostics.push_back("Delphi frame at 0x" +
                              llvm::utohexstr(Frame.Install.InstallVA) +
                              " forwards to an unclassified RTL routine");
      break;
    }

    if (!Routine.Named)
      F.PersonalityName =
          std::string("delphi-") + getDelphiHandlerKindName(Routine.Kind);

    F.Delphi = std::move(Delphi);
    Info.ParseStatus =
        mergeExceptionParseStatus(Info.ParseStatus, F.ParseStatus);
    Info.Functions.push_back(std::move(F));
    ++Added;
  }

  if (Added == 0)
    return;
  Info.addModel(ExceptionModel::WindowsRegistration);
  Info.rebuildIndex();
}

} // namespace neverd::coff_loader
