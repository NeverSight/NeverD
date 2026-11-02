#ifndef NEVERD_SDK_CAPI_OBJCSYNCHRONIZEDSOURCE_H
#define NEVERD_SDK_CAPI_OBJCSYNCHRONIZEDSOURCE_H

#include "ObjCSynchronizedFrameSource.h"
#include "ObjCUnwindSource.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/MachO/DarwinImportVeneer.h"
#include "neverd/loader/ObjC/ObjCCallHints.h"
#include "neverd/sdk/Limits.h"
#include "neverd/support/BranchEncoding.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Endian.h"

#include <algorithm>
#include <capstone/arm64.h>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace neverd::sdk {

struct ObjCSynchronizedSourceLifetime {
  va_t EnterCall = 0;
  va_t ExitCall = 0;
};

struct ObjCSynchronizedSourceProof {
  va_t EnterCall = 0;
  va_t GuardStopCall = 0;
  va_t ExitCall = 0;
  va_t LandingPad = 0;
  va_t ResumeTarget = 0;
  uint8_t UnprotectedReleases = 0;
  bool ReceiverIsSavedLocal = false;
  uint8_t UnprotectedRetains = 0;
  bool ReceiverHasStackCopy = false;
  uint8_t SuspendARC = 0; // release=1, retain=2; all such calls are unprotected
  uint8_t LandingPadPrelude = 0; // exact B to the five-instruction cleanup
  // Separate, non-nested acquisitions of the same saved self receiver.
  std::vector<ObjCSynchronizedSourceLifetime> Lifetimes;
  // Unlocking and resumption-only entries share one authenticated tail.
  bool SplitResumeTail = false;
  va_t LandingPadExitTarget = 0;
  uint8_t SplitResumePadBytes = 0;
  // A CFG proof can have conditional normal exits and repeat acquisitions.
  std::vector<va_t> NormalEnterCalls, NormalExitCalls;
  va_t NormalEnterTarget = 0;
  std::optional<ObjCSynchronizedFrameProof> PrivateFrame;
  bool SuspendDispatchLeave = false;
};

struct ObjCSynchronizedSourceRegion {
  va_t Begin = 0;
  va_t End = 0;
  va_t Landing = 0;
  uint8_t LandingPadPrelude = 0;
};

inline bool objcSynchronizedSourceEHValid(const BinaryImage &Image,
                                          const HighFunc &Function,
                                          bool RequireSelf = true) {
  if (Image.Arch != Arch::AArch64 || Image.Format != BinaryFormat::MachO ||
      Function.StructuredExceptionRegions ||
      Function.UnstructuredExceptionRegions || Function.Params.empty() ||
      (RequireSelf && Function.Params[0].Name != "objc_self") ||
      !Function.ExceptionMetadata)
    return false;
  const auto &EH = *Function.ExceptionMetadata;
  return EH.CodeRange.Begin == Function.Entry && EH.Itanium && EH.ObjC &&
         EH.ParseStatus == ExceptionParseStatus::Complete &&
         EH.Personality == ExceptionPersonality::ObjCPersonalityV0 &&
         EH.Itanium->Actions.empty() && EH.Itanium->TypeTable.empty() &&
         EH.Itanium->ExceptionSpecs.empty() &&
         EH.Itanium->IsCallSiteAddressForm;
}

inline std::optional<ObjCSynchronizedSourceRegion>
objcSynchronizedSourceRegion(const BinaryImage &Image, const HighFunc &Function,
                             bool RequireSelf = true) {
  if (!objcSynchronizedSourceEHValid(Image, Function, RequireSelf))
    return std::nullopt;
  const auto &EH = *Function.ExceptionMetadata;
  if (EH.Itanium->CallSites.size() != 3 || EH.ObjC->LandingPads.size() != 1)
    return std::nullopt;
  const auto &Sites = EH.Itanium->CallSites;
  const auto &Protected = Sites[1];
  const auto &Pad = EH.ObjC->LandingPads.front();
  if (Sites[0].LandingPadVA || Sites[2].LandingPadVA ||
      !Protected.LandingPadVA || Protected.FirstActionOffset ||
      Pad.PadVA != Protected.LandingPadVA ||
      Pad.Kind != ObjCPadKind::SynchronizedExit ||
      Pad.GuardedRange.Begin != Protected.GuardedRange.Begin ||
      Pad.GuardedRange.End != Protected.GuardedRange.End ||
      Sites[0].GuardedRange.Begin != Function.Entry ||
      Sites[0].GuardedRange.End != Protected.GuardedRange.Begin ||
      Sites[2].GuardedRange.Begin != Protected.GuardedRange.End ||
      Sites[2].GuardedRange.End != EH.CodeRange.End ||
      Protected.GuardedRange.Begin < Function.Entry ||
      Protected.GuardedRange.End < Protected.GuardedRange.Begin ||
      Protected.GuardedRange.End > Protected.LandingPadVA ||
      Protected.LandingPadVA > InvalidVA - 20 ||
      EH.CodeRange.End != Protected.LandingPadVA + 20)
    return std::nullopt;
  return ObjCSynchronizedSourceRegion{Protected.GuardedRange.Begin,
                                      Protected.GuardedRange.End,
                                      Protected.LandingPadVA};
}

// A missing call-site-table interval is not a zero-landing-pad call site.
// Accept such an interval only when every instruction decodes and none is a
// call. The complete lifetime proof still checks its control flow and writes.
inline bool objcSynchronizedNonCallGap(const BinaryImage &Image, va_t Begin,
                                       va_t End) {
  if (Image.Arch != Arch::AArch64 || Begin >= End ||
      End - Begin >
          ::neverd::sdk::
              kObjCSynchronizedSourceObjcSynchronizedNonCallGapEndBeginLimit ||
      (Begin | End) % 4)
    return false;
  Decoder Decoder;
  if (!Decoder.init(Arch::AArch64))
    return false;
  for (va_t Address = Begin; Address < End; Address += 4) {
    const uint8_t *Bytes = Image.readVA(Address, 4);
    DecodedInsn Instruction{};
    if (!Bytes || Decoder.decodeOne(Bytes, 4, Address, Instruction) != 4 ||
        !Instruction.Raw ||
        cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_CALL) ||
        cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_INT) ||
        cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_IRET))
      return false;
  }
  return true;
}

// LSDA ranges may share one cleanup, with unprotected instructions between
// them. Preserve those holes rather than treating their union as one protected
// interval. Every table entry and pad record must agree exactly.
inline std::optional<std::vector<ObjCSynchronizedSourceRegion>>
objcSynchronizedInterleavedRanges(const BinaryImage &Image,
                                  const HighFunc &Function) {
  if (!objcSynchronizedSourceEHValid(Image, Function))
    return std::nullopt;
  const auto &EH = *Function.ExceptionMetadata;
  const auto &Sites = EH.Itanium->CallSites;
  if (Sites.size() < 3 ||
      Sites.size() >
          ::neverd::sdk::
              kObjCSynchronizedSourceObjcSynchronizedInterleavedRangesSitesSizeLimit ||
      Sites.front().LandingPadVA || Sites.back().LandingPadVA ||
      EH.CodeRange.End < 20 ||
      Sites.front().GuardedRange.Begin != Function.Entry)
    return std::nullopt;
  const va_t Cleanup = EH.CodeRange.End - 20;
  va_t Landing = InvalidVA;
  for (const auto &Site : Sites)
    if (Site.LandingPadVA)
      Landing = std::min(Landing, Site.LandingPadVA);
  if (Landing < Function.Entry || Landing > Cleanup ||
      (Cleanup - Landing != 0 && Cleanup - Landing != 4) ||
      (Landing | Cleanup) % 4)
    return std::nullopt;
  const uint8_t Prelude = static_cast<uint8_t>(Cleanup - Landing);
  // Clang can give an early range a separate entry consisting solely of
  // B +4. Keep that address as exceptional too; all entries must reach the
  // same exact unlock/resume sequence, with no intervening effects.
  if (Prelude) {
    const uint8_t *Bytes = Image.readVA(Landing, 4);
    if (!Bytes || llvm::support::endian::read32le(Bytes) != 0x14000001U)
      return std::nullopt;
  }
  std::vector<ObjCSynchronizedSourceRegion> Regions;
  va_t Next = Function.Entry;
  for (const auto &Site : Sites) {
    const auto &Range = Site.GuardedRange;
    if (Range.Begin < Next || Range.End <= Range.Begin ||
        Range.End > EH.CodeRange.End || (Range.Begin | Range.End) % 4 ||
        Site.FirstActionOffset)
      return std::nullopt;
    if (Range.Begin != Next &&
        !objcSynchronizedNonCallGap(Image, Next, Range.Begin))
      return std::nullopt;
    Next = Range.End;
    if (!Site.LandingPadVA)
      continue;
    if (Site.LandingPadVA != Landing && Site.LandingPadVA != Cleanup)
      return std::nullopt;
    if (Range.End > Landing ||
        std::count_if(EH.ObjC->LandingPads.begin(), EH.ObjC->LandingPads.end(),
                      [&](const auto &Pad) {
                        const bool KindMatches =
                            Pad.PadVA == Cleanup
                                ? Pad.Kind == ObjCPadKind::SynchronizedExit
                                : Pad.Kind == ObjCPadKind::Cleanup ||
                                      Pad.Kind == ObjCPadKind::SynchronizedExit;
                        return Pad.PadVA == Site.LandingPadVA && KindMatches &&
                               Pad.Catches.empty() &&
                               Pad.GuardedRange.Begin == Range.Begin &&
                               Pad.GuardedRange.End == Range.End;
                      }) != 1)
      return std::nullopt;
    Regions.push_back({Range.Begin, Range.End, Landing, Prelude});
  }
  if (Regions.empty() || Regions.size() != EH.ObjC->LandingPads.size() ||
      Next != EH.CodeRange.End || !Landing)
    return std::nullopt;
  return Regions;
}

inline std::optional<uint32_t> objcSynchronizedWord(const BinaryImage &Image,
                                                    va_t Address) {
  const uint8_t *Bytes = Image.readVA(Address, 4);
  if (!Bytes)
    return std::nullopt;
  return llvm::support::endian::read32le(Bytes);
}

inline va_t objcSynchronizedBranchTarget(const BinaryImage &Image,
                                         va_t Address) {
  const auto Instruction = objcSynchronizedWord(Image, Address);
  if (!Instruction || !branch::A64BranchLink.matches(*Instruction))
    return 0;
  return branch::a64BranchTarget(*Instruction, Address).value_or(0);
}

inline bool objcSynchronizedCallIs(const BinaryImage &Image, va_t Address,
                                   llvm::StringRef Name) {
  const va_t Target = objcSynchronizedBranchTarget(Image, Address);
  return Target && llvm::StringRef(Image.getFunctionNameAt(Target)) == Name;
}

inline std::optional<va_t>
objcSynchronizedDirectBranchTarget(const DecodedInsn &Instruction) {
  if (!Instruction.Raw || !Instruction.Raw->detail)
    return std::nullopt;
  switch (Instruction.Id) {
  case ARM64_INS_B:
  case ARM64_INS_CBZ:
  case ARM64_INS_CBNZ:
  case ARM64_INS_TBZ:
  case ARM64_INS_TBNZ:
    break;
  default:
    return std::nullopt;
  }
  const auto &Operands = Instruction.Raw->detail->aarch64;
  if (!Operands.op_count)
    return std::nullopt;
  const auto &Target = Operands.operands[Operands.op_count - 1];
  if (Target.type != AARCH64_OP_IMM || Target.imm < 0 || Target.imm % 4)
    return std::nullopt;
  return static_cast<va_t>(Target.imm);
}

inline bool objcSynchronizedForwardBranch(const DecodedInsn &Instruction,
                                          va_t LastTarget,
                                          va_t FirstTarget = 0) {
  const auto Target = objcSynchronizedDirectBranchTarget(Instruction);
  return Target && *Target > Instruction.Addr && *Target >= FirstTarget &&
         *Target <= LastTarget;
}

// A narrowly recognized clang @synchronized cleanup: the Itanium call-site
// table names one unconditional pad, and that pad does nothing except unlock
// the saved receiver and resume the original exception. Its source equivalent
// is a C cleanup variable compiled with -fexceptions. In particular, do not
// accept a pad which also releases an ARC object or executes a finally body.
inline std::optional<ObjCSynchronizedSourceProof>
proveObjCSynchronizedStackReceiverCleanup(const BinaryImage &Image,
                                          const HighFunc &Function) {
  const auto Region = objcSynchronizedSourceRegion(Image, Function);
  if (!Region)
    return std::nullopt;

  const auto Word = [&](va_t Address) {
    return objcSynchronizedWord(Image, Address);
  };
  const auto HasCall = [&](va_t Address, llvm::StringRef Name) {
    return objcSynchronizedCallIs(Image, Address, Name);
  };
  const va_t Landing = Region->Landing;
  if (Word(Landing) != 0xaa0003f3U || Word(Landing + 4) != 0xf94007e0U ||
      !HasCall(Landing + 8, "_objc_sync_exit") ||
      Word(Landing + 12) != 0xaa1303e0U ||
      !HasCall(Landing + 16, "__Unwind_Resume"))
    return std::nullopt;

  // The source guard uses objc_self. Prove the original enter, normal exit,
  // and exceptional exit all load that same saved receiver, and that no
  // later instruction stores a new value into its stack slot.
  if (Word(Function.Entry + 16) != 0xf90007e0U ||
      Word(Function.Entry + 24) != 0xf94007e0U)
    return std::nullopt;
  const va_t EnterCall = Function.Entry + 28;
  if (!HasCall(EnterCall, "_objc_sync_enter") || Region->Begin != EnterCall + 4)
    return std::nullopt;
  // The one call after the LSDA range is dispatch_group_enter. Clear the C
  // cleanup guard before it: the original unwind table does not cover it.
  const va_t Unprotected = Region->End;
  const va_t DispatchCall = Unprotected + 8;
  const va_t ExitCall = Unprotected + 16;
  if (Unprotected > InvalidVA - 40 || Landing != Unprotected + 40 ||
      Word(Unprotected) != 0xf94007e0U ||
      Word(Unprotected + 4) != 0xf9400400U ||
      !HasCall(DispatchCall, "_dispatch_group_enter") ||
      Word(Unprotected + 12) != 0xf94007e0U ||
      !HasCall(ExitCall, "_objc_sync_exit"))
    return std::nullopt;
  for (va_t Address = Function.Entry + 20; Address < Landing; Address += 4)
    if (Word(Address) == 0xf90007e0U)
      return std::nullopt;
  return ObjCSynchronizedSourceProof{
      EnterCall, DispatchCall, ExitCall, Landing,
      objcSynchronizedBranchTarget(Image, Landing + 16)};
}

// Reuse the loader's ARC ABI catalog, including register-specific veneers.
// A matching symbol spelling alone cannot establish the runtime operation.
inline std::optional<SourceCallTypeHint>
objcSynchronizedRuntimeTarget(const BinaryImage &Image, va_t Target) {
  const auto Slot = darwinImportVeneerSlot(Image, Target);
  const auto Hint =
      Slot ? objcRuntimeSourceCallHint(Image, *Slot) : std::nullopt;
  if (!Hint || Hint->Signature.Parameters.size() != 1)
    return std::nullopt;
  const auto Bind = Image.DyldBindSlots.find(*Slot);
  if (Bind == Image.DyldBindSlots.end() ||
      Bind->second.Module != "/usr/lib/libobjc.A.dylib" ||
      std::find(Image.DynInfo.NeededLibs.begin(),
                Image.DynInfo.NeededLibs.end(),
                Bind->second.Module) == Image.DynInfo.NeededLibs.end())
    return std::nullopt;
  const auto &Location = Hint->Signature.Parameters.front().Location;
  if (Location.Kind != SourceABICarrierKind::IntegerRegister ||
      Location.ValueBytes != 8)
    return std::nullopt;
  return Hint;
}

inline bool objcSynchronizedRuntimeTargetIs(const BinaryImage &Image,
                                            va_t Target, llvm::StringRef Name,
                                            uint64_t ArgumentRegister) {
  const auto Hint = objcSynchronizedRuntimeTarget(Image, Target);
  return Hint && Hint->TargetName == Name &&
         Hint->Signature.Parameters.front().Location.RegisterOffset ==
             ArgumentRegister;
}

// Clang may synchronize on a retained getter result kept in a private stack
// slot, then call the getter again for the mutation. These objects need not
// be equal. Check the entire small frame so neither a stack alias nor a
// different result can replace the lock used by either exit.
inline std::optional<ObjCSynchronizedSourceProof>
proveObjCSynchronizedRetainedStackReceiverCleanup(const BinaryImage &Image,
                                                  const HighFunc &Function) {
  const auto Region = objcSynchronizedSourceRegion(Image, Function);
  const va_t Entry = Function.Entry;
  if (!Region || Function.Params.size() != 2 || Entry > InvalidVA - 0x74 ||
      Entry % 4 || Region->Begin != Entry + 0x28 ||
      Region->End != Entry + 0x40 || Region->Landing != Entry + 0x60)
    return std::nullopt;
  const auto Word = [&](unsigned Offset) {
    return objcSynchronizedWord(Image, Entry + Offset);
  };
  for (const auto &[Offset, Expected] :
       {std::pair<unsigned, uint32_t>{0x00, 0xd100c3ffU}, // sub sp, #0x30
        {0x04, 0xa9014ff4U},
        {0x08, 0xa9027bfdU},
        {0x0c, 0x910083fdU},
        {0x10, 0xaa0003f3U}, // save incoming self
        {0x18, 0xaa1d03fdU},
        {0x20, 0xf90007e0U}, // save retained lock
        {0x28, 0xaa1303e0U},
        {0x30, 0xaa1d03fdU},
        {0x38, 0xaa0003f3U}, // preserve the second getter's retained result
        {0x44, 0xf94007e0U},
        {0x4c, 0xf94007e0U}, // reload original lock
        {0x50, 0xa9427bfdU},
        {0x54, 0xa9414ff4U},
        {0x58, 0x9100c3ffU},
        {0x60, 0xaa0003f3U}, // save exception
        {0x64, 0xf94007e0U},
        {0x6c, 0xaa1303e0U}})
    if (Word(Offset) != Expected)
      return std::nullopt;
  const auto Target = [&](unsigned Offset) {
    return objcSynchronizedBranchTarget(Image, Entry + Offset);
  };
  const auto RuntimeCall = [&](unsigned Offset, llvm::StringRef Name,
                               uint64_t Argument) {
    return objcSynchronizedRuntimeTargetIs(Image, Target(Offset), Name,
                                           Argument);
  };
  const va_t Getter = Target(0x14);
  const va_t Mutation = Target(0x3c);
  const auto Tail = Word(0x5c);
  if (!Getter || Getter != Target(0x2c) || !Mutation ||
      !objcSelectorStubPreservesNonvolatileRegisters(Image, Getter) ||
      !objcSelectorStubPreservesNonvolatileRegisters(Image, Mutation) ||
      !RuntimeCall(0x1c, "objc_retainAutoreleasedReturnValue", a64reg::X0) ||
      !RuntimeCall(0x34, "objc_retainAutoreleasedReturnValue", a64reg::X0) ||
      !RuntimeCall(0x40, "objc_release", a64reg::X19) ||
      !objcSynchronizedCallIs(Image, Entry + 0x24, "_objc_sync_enter") ||
      !objcSynchronizedCallIs(Image, Entry + 0x48, "_objc_sync_exit") ||
      !objcSynchronizedCallIs(Image, Entry + 0x68, "_objc_sync_exit") ||
      !objcSynchronizedCallIs(Image, Entry + 0x70, "__Unwind_Resume") ||
      !Tail || !branch::A64Branch.matches(*Tail) ||
      !objcSynchronizedRuntimeTargetIs(
          Image, branch::a64BranchTarget(*Tail, Entry + 0x5c).value_or(0),
          "objc_release", a64reg::X0))
    return std::nullopt;
  return ObjCSynchronizedSourceProof{Entry + 0x24, Entry + 0x40,
                                     Entry + 0x48, Entry + 0x60,
                                     Target(0x70), 1,
                                     true,         0,
                                     true};
}

// A second clang shape keeps the receiver in a callee-saved register. Forward
// branches in the prefix must join before loading the enter argument; forward
// branches in the protected body must join by its end. No path may bypass the
// lock, reach its pad normally, or skip the first unprotected call. The C guard
// is cleared before that call, preserving the original LSDA boundary.
inline std::optional<ObjCSynchronizedSourceProof>
proveObjCSynchronizedRegisterReceiverCleanup(const BinaryImage &Image,
                                             const HighFunc &Function) {
  const auto Region = objcSynchronizedSourceRegion(Image, Function, false);
  if (!Region || Function.Entry > InvalidVA - 8 ||
      Region->Begin < Function.Entry + 8 || Region->Landing < 20 ||
      Region->End > Region->Landing - 20 || Region->Begin > Region->End ||
      (Function.Entry | Region->Begin | Region->End | Region->Landing) % 4 ||
      Region->End - Function.Entry >
          ::neverd::sdk::
              kObjCSynchronizedSourceProveObjCSynchronizedRegisterReceiverCleanupRegionEndFunctionEntryLimit)
    return std::nullopt;
  const auto Word = [&](va_t Address) {
    return objcSynchronizedWord(Image, Address);
  };
  const auto HasCall = [&](va_t Address, llvm::StringRef Name) {
    return objcSynchronizedCallIs(Image, Address, Name);
  };
  // LSDA ranges begin at potentially throwing instructions. Clang may put
  // straight-line non-call preparation between the enter and that range.
  va_t EnterCall = 0;
  for (
      va_t Address = Region->Begin - 4;
      Address >= Function.Entry + 4 &&
      Region->Begin - Address <=
          ::neverd::sdk::
              kObjCSynchronizedSourceProveObjCSynchronizedRegisterReceiverCleanupRegionBeginAddressLimit;
      Address -= 4) {
    if (HasCall(Address, "_objc_sync_enter")) {
      EnterCall = Address;
      break;
    }
  }
  if (!EnterCall)
    return std::nullopt;
  const va_t Landing = Region->Landing;
  const auto SaveX0 = [](unsigned Register) { return 0xaa0003e0U | Register; };
  const auto LoadX0 = [](unsigned Register) {
    return 0xaa0003e0U | (Register << 16);
  };
  unsigned ReceiverRegister = 0;
  bool ReceiverFromSelf = false, ReceiverFromRetainResult = false;
  bool ReceiverFromRetainedLoad = false;
  for (unsigned Register = 19; Register <= 22; ++Register) {
    if (Word(EnterCall - 4) == LoadX0(Register)) {
      const auto Load = EnterCall >= Function.Entry + 12 ? Word(EnterCall - 12)
                                                         : std::nullopt;
      // A block can lock a global object rather than its incoming context.
      // LDR Xn followed by the authenticated retain-Xn veneer defines that
      // receiver independently of X0. The load remains an ordinary source
      // memory access; the proof never substitutes the block or reloads the
      // global when unlocking.
      if (Load && (*Load & 0xffc0001fU) == (0xf9400000U | Register) &&
          ((*Load >> 5) & 31) != 31 &&
          objcSynchronizedRuntimeTargetIs(
              Image, objcSynchronizedBranchTarget(Image, EnterCall - 8),
              "objc_retain", a64reg::X0 + Register * 8)) {
        ReceiverRegister = Register;
        ReceiverFromRetainedLoad = true;
      } else if (Function.Params[0].Name == "objc_self") {
        ReceiverRegister = Register;
        ReceiverFromSelf = true;
      }
    } else if (Word(EnterCall - 4) == SaveX0(Register) &&
               EnterCall >= Function.Entry + 8 &&
               HasCall(EnterCall - 8, "_objc_retainAutoreleasedReturnValue")) {
      ReceiverRegister = Register;
      ReceiverFromRetainResult = true;
    }
  }
  if (!ReceiverRegister)
    return std::nullopt;
  unsigned ExceptionRegister = 0;
  for (unsigned Register = 19; Register <= 22; ++Register)
    if (Register != ReceiverRegister && Word(Landing) == SaveX0(Register))
      ExceptionRegister = Register;
  if (!ExceptionRegister)
    return std::nullopt;
  va_t Normal = Region->End;
  if (Word(Normal) == SaveX0(20) || Word(Normal) == SaveX0(21) ||
      Word(Normal) == SaveX0(22))
    Normal += 4; // retain an already computed result across ARC releases
  va_t FirstRetain = 0;
  uint8_t UnprotectedRetains = 0;
  if ((Word(Normal) == 0xaa1403e0U || Word(Normal) == 0xaa1503e0U ||
       Word(Normal) == 0xaa1603e0U) &&
      HasCall(Normal + 4, "_objc_retain")) {
    FirstRetain = Normal + 4;
    UnprotectedRetains = 1;
    Normal += 8;
  }
  va_t FirstRelease = 0;
  uint8_t UnprotectedReleases = 0;
  while (UnprotectedReleases < 2 && Normal < Landing - 8) {
    const bool Ordinary =
        (Word(Normal) == 0xaa1403e0U || Word(Normal) == 0xaa1503e0U ||
         Word(Normal) == 0xaa1603e0U) &&
        HasCall(Normal + 4, "_objc_release");
    const auto Runtime =
        ReceiverFromRetainedLoad
            ? objcSynchronizedRuntimeTarget(
                  Image, objcSynchronizedBranchTarget(Image, Normal))
            : std::nullopt;
    const bool RegisterRelease =
        Runtime && Runtime->TargetName == "objc_release" &&
        Runtime->Signature.Parameters.front().Location.RegisterOffset >=
            a64reg::X20 &&
        Runtime->Signature.Parameters.front().Location.RegisterOffset <=
            a64reg::X22;
    if (!Ordinary && !RegisterRelease)
      break;
    if (!FirstRelease)
      FirstRelease = Normal + (Ordinary ? 4 : 0);
    ++UnprotectedReleases;
    Normal += Ordinary ? 8 : 4;
  }
  const va_t ExitCall = Normal + 4;
  if (!HasCall(EnterCall, "_objc_sync_enter") || Normal >= Landing - 4 ||
      Word(Normal) != LoadX0(ReceiverRegister) ||
      !HasCall(ExitCall, "_objc_sync_exit") ||
      Word(Landing + 4) != LoadX0(ReceiverRegister) ||
      !HasCall(Landing + 8, "_objc_sync_exit") ||
      Word(Landing + 12) != LoadX0(ExceptionRegister) ||
      !HasCall(Landing + 16, "__Unwind_Resume"))
    return std::nullopt;

  if (ReceiverFromRetainedLoad &&
      (!objcUnwindRuntimeTargetIs(
           Image, objcSynchronizedBranchTarget(Image, EnterCall),
           "_objc_sync_enter", "/usr/lib/libobjc.A.dylib") ||
       !objcUnwindRuntimeTargetIs(
           Image, objcSynchronizedBranchTarget(Image, ExitCall),
           "_objc_sync_exit", "/usr/lib/libobjc.A.dylib") ||
       !objcUnwindRuntimeTargetIs(
           Image, objcSynchronizedBranchTarget(Image, Landing + 8),
           "_objc_sync_exit", "/usr/lib/libobjc.A.dylib") ||
       !objcUnwindRuntimeTargetIs(
           Image, objcSynchronizedBranchTarget(Image, Landing + 16),
           "__Unwind_Resume",
           "/usr/lib/libSystem.B.dylib|/usr/lib/system/libunwind.dylib")))
    return std::nullopt;
  va_t SavedReceiver = ReceiverFromRetainedLoad   ? EnterCall - 12
                       : ReceiverFromRetainResult ? EnterCall - 4
                                                  : 0;
  if (ReceiverFromSelf) {
    for (va_t Address = Function.Entry; Address + 4 < EnterCall; Address += 4) {
      if (Word(Address) != SaveX0(ReceiverRegister))
        continue;
      if (SavedReceiver || Address > Function.Entry + 32)
        return std::nullopt;
      SavedReceiver = Address;
    }
    if (!SavedReceiver)
      return std::nullopt;
  }

  Decoder Decoder;
  if (!Decoder.init(Arch::AArch64))
    return std::nullopt;
  const unsigned ReceiverXRegisters[] = {ARM64_REG_X19, ARM64_REG_X20,
                                         ARM64_REG_X21, ARM64_REG_X22};
  const unsigned ReceiverWRegisters[] = {ARM64_REG_W19, ARM64_REG_W20,
                                         ARM64_REG_W21, ARM64_REG_W22};
  const unsigned ReceiverX = ReceiverXRegisters[ReceiverRegister - 19];
  const unsigned ReceiverW = ReceiverWRegisters[ReceiverRegister - 19];
  for (va_t Address = Function.Entry; Address < ExitCall; Address += 4) {
    const uint8_t *Bytes = Image.readVA(Address, 4);
    DecodedInsn Instruction{};
    if (!Bytes || Decoder.decodeOne(Bytes, 4, Address, Instruction) != 4 ||
        !Instruction.Raw)
      return std::nullopt;
    cs_regs Reads{}, Writes{};
    uint8_t ReadCount = 0, WriteCount = 0;
    if (cs_regs_access(Decoder.getHandle(), Instruction.Raw, Reads, &ReadCount,
                       Writes, &WriteCount) != CS_ERR_OK)
      return std::nullopt;
    for (uint8_t I = 0; I < WriteCount; ++I) {
      if (ReceiverFromSelf && Address < SavedReceiver &&
          (Writes[I] == ARM64_REG_X0 || Writes[I] == ARM64_REG_W0))
        return std::nullopt;
      if (Address > SavedReceiver &&
          (Writes[I] == ReceiverX || Writes[I] == ReceiverW))
        return std::nullopt;
    }
    const bool IsJump =
        cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_JUMP);
    const bool IsCall =
        cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_CALL);
    if (cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_RET) ||
        cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_IRET) ||
        cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_INT))
      return std::nullopt;
    if (ReceiverFromSelf && Address < SavedReceiver && (IsJump || IsCall))
      return std::nullopt;
    if (IsJump) {
      if (ReceiverFromRetainedLoad && Address < EnterCall)
        return std::nullopt;
      if (Address < EnterCall) {
        const va_t Join = ReceiverFromSelf ? EnterCall - 4 : EnterCall - 8;
        if (!objcSynchronizedForwardBranch(Instruction, Join))
          return std::nullopt;
      } else if (Address < Region->Begin || Address >= Region->End ||
                 !objcSynchronizedForwardBranch(Instruction, Region->End))
        return std::nullopt;
    }
    if (Address > EnterCall && Address < Region->Begin && IsCall)
      return std::nullopt;
    if ((Address != EnterCall && HasCall(Address, "_objc_sync_enter")) ||
        HasCall(Address, "_objc_sync_exit"))
      return std::nullopt;
    if (UnprotectedReleases && Address > EnterCall && Address < Region->End &&
        HasCall(Address, "_objc_release"))
      return std::nullopt;
    if (ReceiverFromRetainedLoad && UnprotectedReleases &&
        Address > EnterCall && Address < Region->End) {
      const auto Runtime = objcSynchronizedRuntimeTarget(
          Image, objcSynchronizedBranchTarget(Image, Address));
      if (Runtime && Runtime->TargetName == "objc_release")
        return std::nullopt;
    }
    if (UnprotectedRetains && Address > EnterCall && Address < Region->End &&
        HasCall(Address, "_objc_retain"))
      return std::nullopt;
  }
  return ObjCSynchronizedSourceProof{
      EnterCall,
      UnprotectedRetains    ? FirstRetain
      : UnprotectedReleases ? FirstRelease
                            : ExitCall,
      ExitCall,
      Landing,
      objcSynchronizedBranchTarget(Image, Landing + 16),
      UnprotectedReleases,
      ReceiverFromRetainResult || ReceiverFromRetainedLoad,
      UnprotectedRetains};
}

// The token mutators skip the lock entirely for a null argument. On the
// non-null path clang keeps that argument in x19, the retained lock in x20,
// and the receiver in x21. Only the exact forward skip and one protected,
// straight-line mutation may use this cleanup source guard.
inline std::optional<ObjCSynchronizedSourceProof>
proveObjCSynchronizedBranchedLocalReceiverCleanup(const BinaryImage &Image,
                                                  const HighFunc &Function) {
  const auto Region = objcSynchronizedSourceRegion(Image, Function);
  if (!Region || Function.Entry > InvalidVA - 0xbc ||
      Region->Begin != Function.Entry + 0x4c ||
      Region->End != Function.Entry + 0x68 ||
      Region->Landing != Function.Entry + 0xa8)
    return std::nullopt;
  const va_t Entry = Function.Entry;
  const va_t End = Region->End;
  const va_t Landing = Region->Landing;
  const auto Word = [&](va_t Address) {
    return objcSynchronizedWord(Image, Address);
  };
  const auto HasCall = [&](va_t Address, llvm::StringRef Name) {
    return objcSynchronizedCallIs(Image, Address, Name);
  };
  const auto Tail = Word(End + 60);
  if (!Tail || !branch::A64Branch.matches(*Tail))
    return std::nullopt;
  const va_t ReleaseTarget = objcSynchronizedBranchTarget(Image, End + 4);
  if (!ReleaseTarget ||
      branch::a64BranchTarget(*Tail, End + 60) != ReleaseTarget)
    return std::nullopt;
  if (Word(Entry) != 0xa9bd57f6U || Word(Entry + 4) != 0xa9014ff4U ||
      Word(Entry + 8) != 0xa9027bfdU || Word(Entry + 12) != 0x910083fdU ||
      Word(Entry + 16) != 0xaa0203f3U || // save argument in x19
      Word(Entry + 20) != 0xaa0003f5U || // save objc_self in x21
      Word(Entry + 24) != 0xaa0203e0U ||
      !HasCall(Entry + 0x1c, "_objc_retain") ||
      Word(Entry + 0x30) != 0xb40002b3U || // cbz x19, after unlock
      Word(Entry + 0x34) != 0xaa1503e0U ||
      !HasCall(Entry + 0x38, "_objc_msgSend$runningTokens") ||
      Word(Entry + 0x3c) != 0xaa1d03fdU ||
      !HasCall(Entry + 0x40, "_objc_retainAutoreleasedReturnValue") ||
      Word(Entry + 0x44) != 0xaa0003f4U || // retained lock in x20
      !HasCall(Entry + 0x48, "_objc_sync_enter") ||
      Word(Entry + 0x4c) != 0xaa1503e0U ||
      !HasCall(Entry + 0x50, "_objc_msgSend$runningTokens") ||
      !HasCall(Entry + 0x58, "_objc_retainAutoreleasedReturnValue") ||
      Word(Entry + 0x5c) != 0xaa0003f5U || // retained mutation receiver
      Word(Entry + 0x60) != 0xaa1303e2U ||
      (!HasCall(Entry + 0x64, "_objc_msgSend$addObject:") &&
       !HasCall(Entry + 0x64, "_objc_msgSend$removeObject:")) ||
      Word(End) != 0xaa1503e0U || !HasCall(End + 4, "_objc_release") ||
      Word(End + 8) != 0xaa1403e0U || !HasCall(End + 12, "_objc_sync_exit") ||
      Word(End + 16) != 0xaa1403e0U || !HasCall(End + 20, "_objc_release") ||
      Word(End + 24) != 0x14000005U || // normal path joins after skip
      Word(End + 44) != 0xaa1303e0U || Word(End + 48) != 0xa9427bfdU ||
      Word(End + 52) != 0xa9414ff4U || Word(End + 56) != 0xa8c357f6U ||
      Word(Landing) != 0xaa0003f3U || // preserve exception in x19
      Word(Landing + 4) != 0xaa1403e0U ||
      !HasCall(Landing + 8, "_objc_sync_exit") ||
      Word(Landing + 12) != 0xaa1303e0U ||
      !HasCall(Landing + 16, "__Unwind_Resume"))
    return std::nullopt;

  Decoder Decoder;
  if (!Decoder.init(Arch::AArch64))
    return std::nullopt;
  for (va_t Address = Entry; Address < End + 44; Address += 4) {
    const uint8_t *Bytes = Image.readVA(Address, 4);
    DecodedInsn Instruction{};
    if (!Bytes || Decoder.decodeOne(Bytes, 4, Address, Instruction) != 4 ||
        !Instruction.Raw)
      return std::nullopt;
    cs_regs Reads{}, Writes{};
    uint8_t ReadCount = 0, WriteCount = 0;
    if (cs_regs_access(Decoder.getHandle(), Instruction.Raw, Reads, &ReadCount,
                       Writes, &WriteCount) != CS_ERR_OK)
      return std::nullopt;
    for (uint8_t I = 0; I < WriteCount; ++I) {
      if ((Address > Entry + 16 &&
           (Writes[I] == ARM64_REG_X19 || Writes[I] == ARM64_REG_W19)) ||
          (((Address > Entry + 20 && Address < Entry + 0x4c) ||
            (Address > Entry + 0x5c)) &&
           (Writes[I] == ARM64_REG_X21 || Writes[I] == ARM64_REG_W21)) ||
          (Address > Entry + 0x44 &&
           (Writes[I] == ARM64_REG_X20 || Writes[I] == ARM64_REG_W20)))
        return std::nullopt;
    }
    if (cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_RET))
      return std::nullopt;
    if (cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_JUMP) &&
        Address != Entry + 0x30 && Address != End + 24)
      return std::nullopt;
    if (Address >= End + 28 &&
        cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_CALL))
      return std::nullopt;
  }
  return ObjCSynchronizedSourceProof{
      Entry + 0x48,
      End + 4,
      End + 12,
      Landing,
      objcSynchronizedBranchTarget(Image, Landing + 16),
      1,
      true};
}

// Normal cleanup tails can have a shared epilogue before a bypass block in
// address order. Prove the complete suffix graph is acyclic and every edge
// stays after the normal unlock or terminates with a return/external tail.
// This permits backward joins without permitting a loop, pad entry or relock.
inline bool objcSynchronizedNormalSuffixValid(const BinaryImage &Image,
                                              va_t Entry, va_t Begin,
                                              va_t Landing,
                                              uint8_t LandingPadPrelude = 0) {
  if (Begin < Entry || Begin >= Landing ||
      Landing - Entry >
          ::neverd::sdk::
              kObjCSynchronizedSourceObjcSynchronizedNormalSuffixValidLandingEntryLimit ||
      (LandingPadPrelude != 0 && LandingPadPrelude != 4) ||
      Landing > InvalidVA - 20 - LandingPadPrelude || (Begin | Landing) % 4)
    return false;
  Decoder Decoder;
  if (!Decoder.init(Arch::AArch64))
    return false;
  const size_t Count = (Landing - Begin) / 4;
  std::vector<std::vector<size_t>> Edges(Count);
  std::vector<unsigned> Incoming(Count, 0);
  const auto AddEdge = [&](va_t Address, va_t Target) {
    if (Target < Begin || Target >= Landing || Target % 4)
      return false;
    const size_t Next = (Target - Begin) / 4;
    Edges[(Address - Begin) / 4].push_back(Next);
    ++Incoming[Next];
    return true;
  };
  for (va_t Address = Begin; Address < Landing; Address += 4) {
    const uint8_t *Bytes = Image.readVA(Address, 4);
    DecodedInsn Instruction{};
    if (!Bytes || Decoder.decodeOne(Bytes, 4, Address, Instruction) != 4 ||
        !Instruction.Raw ||
        objcSynchronizedCallIs(Image, Address, "_objc_sync_enter") ||
        objcSynchronizedCallIs(Image, Address, "_objc_sync_exit") ||
        objcSynchronizedCallIs(Image, Address, "__Unwind_Resume"))
      return false;
    const auto Word = objcSynchronizedWord(Image, Address);
    if (Word == 0xd65f03c0U)
      continue;
    if (cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_RET) ||
        cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_IRET) ||
        cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_INT))
      return false;
    if (cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_JUMP)) {
      const auto Target = objcSynchronizedDirectBranchTarget(Instruction);
      if (!Target)
        return false;
      const bool Unconditional = Word && branch::A64Branch.matches(*Word);
      if (Unconditional &&
          (*Target < Entry || *Target >= Landing + 20 + LandingPadPrelude))
        continue;
      if (!AddEdge(Address, *Target))
        return false;
      if (Unconditional)
        continue;
    }
    if (!AddEdge(Address, Address + 4))
      return false;
  }
  std::vector<size_t> Ready;
  for (size_t I = 0; I < Count; ++I)
    if (!Incoming[I])
      Ready.push_back(I);
  for (size_t I = 0; I < Ready.size(); ++I)
    for (size_t Next : Edges[Ready[I]])
      if (!--Incoming[Next])
        Ready.push_back(Next);
  return Ready.size() == Count;
}

inline std::optional<ObjCSynchronizedSourceProof>
proveObjCSynchronizedInterleavedCleanup(const BinaryImage &Image,
                                        const HighFunc &Function) {
  const auto Regions = objcSynchronizedInterleavedRanges(Image, Function);
  if (!Regions || Function.Entry > InvalidVA - 8 ||
      Regions->front().Begin < Function.Entry + 8 ||
      Regions->back().End > Regions->front().Landing ||
      Regions->front().Landing - Function.Entry >
          ::neverd::sdk::
              kObjCSynchronizedSourceProveObjCSynchronizedInterleavedCleanupRegionsFrontLandingFunctionEntryLimit)
    return std::nullopt;
  const va_t Landing = Regions->front().Landing;
  const uint8_t Prelude = Regions->front().LandingPadPrelude;
  const va_t Cleanup = Landing + Prelude;
  const auto Word = [&](va_t Address) {
    return objcSynchronizedWord(Image, Address);
  };
  const auto HasCall = [&](va_t Address, llvm::StringRef Name) {
    return objcSynchronizedCallIs(Image, Address, Name);
  };
  va_t EnterCall = 0;
  for (
      va_t Address = Regions->front().Begin - 4;
      Address >= Function.Entry + 4 &&
      Regions->front().Begin - Address <=
          ::neverd::sdk::
              kObjCSynchronizedSourceProveObjCSynchronizedInterleavedCleanupRegionsFrontBeginAddressLimit;
      Address -= 4)
    if (HasCall(Address, "_objc_sync_enter")) {
      EnterCall = Address;
      break;
    }
  if (!EnterCall)
    return std::nullopt;
  const auto SaveX0 = [](unsigned Register) { return 0xaa0003e0U | Register; };
  const auto LoadX0 = [](unsigned Register) {
    return 0xaa0003e0U | (Register << 16);
  };
  unsigned ReceiverRegister = 0, ExceptionRegister = 0;
  bool ReceiverFromRetainResult = false;
  for (unsigned Register = 19; Register <= 22; ++Register)
    if (Word(EnterCall - 4) == LoadX0(Register))
      ReceiverRegister = Register;
    else if (EnterCall >= Function.Entry + 8 &&
             Word(EnterCall - 4) == SaveX0(Register) &&
             objcSynchronizedRuntimeTargetIs(
                 Image, objcSynchronizedBranchTarget(Image, EnterCall - 8),
                 "objc_retainAutoreleasedReturnValue", a64reg::X0)) {
      ReceiverRegister = Register;
      ReceiverFromRetainResult = true;
    }
  for (unsigned Register = 19; Register <= 22; ++Register)
    if (Register != ReceiverRegister && Word(Cleanup) == SaveX0(Register))
      ExceptionRegister = Register;
  if (!ReceiverRegister || !ExceptionRegister ||
      Word(Cleanup + 4) != LoadX0(ReceiverRegister) ||
      !HasCall(Cleanup + 8, "_objc_sync_exit") ||
      Word(Cleanup + 12) != LoadX0(ExceptionRegister) ||
      !HasCall(Cleanup + 16, "__Unwind_Resume"))
    return std::nullopt;
  va_t SavedReceiver = ReceiverFromRetainResult ? EnterCall - 4 : 0;
  va_t ExitCall = 0;
  for (va_t Address = Function.Entry; Address < Landing; Address += 4) {
    if (!ReceiverFromRetainResult && Address + 4 < EnterCall &&
        Word(Address) == SaveX0(ReceiverRegister)) {
      if (SavedReceiver || Address > Function.Entry + 32)
        return std::nullopt;
      SavedReceiver = Address;
    }
    if (HasCall(Address, "_objc_sync_exit")) {
      if (ExitCall || Address < Regions->back().End ||
          Word(Address - 4) != LoadX0(ReceiverRegister))
        return std::nullopt;
      ExitCall = Address;
    }
  }
  if (!SavedReceiver || !ExitCall)
    return std::nullopt;

  Decoder Decoder;
  if (!Decoder.init(Arch::AArch64))
    return std::nullopt;
  const unsigned ReceiverXRegisters[] = {ARM64_REG_X19, ARM64_REG_X20,
                                         ARM64_REG_X21, ARM64_REG_X22};
  const unsigned ReceiverWRegisters[] = {ARM64_REG_W19, ARM64_REG_W20,
                                         ARM64_REG_W21, ARM64_REG_W22};
  uint8_t UnprotectedARC = 0, ProtectedARC = 0;
  bool BypassesLock = false;
  for (va_t Address = Function.Entry; Address < ExitCall; Address += 4) {
    const uint8_t *Bytes = Image.readVA(Address, 4);
    DecodedInsn Instruction{};
    if (!Bytes || Decoder.decodeOne(Bytes, 4, Address, Instruction) != 4 ||
        !Instruction.Raw)
      return std::nullopt;
    cs_regs Reads{}, Writes{};
    uint8_t ReadCount = 0, WriteCount = 0;
    if (cs_regs_access(Decoder.getHandle(), Instruction.Raw, Reads, &ReadCount,
                       Writes, &WriteCount) != CS_ERR_OK)
      return std::nullopt;
    for (uint8_t I = 0; I < WriteCount; ++I)
      if ((!ReceiverFromRetainResult && Address < SavedReceiver &&
           (Writes[I] == ARM64_REG_X0 || Writes[I] == ARM64_REG_W0)) ||
          (Address > SavedReceiver &&
           (Writes[I] == ReceiverXRegisters[ReceiverRegister - 19] ||
            Writes[I] == ReceiverWRegisters[ReceiverRegister - 19])))
        return std::nullopt;
    const bool IsJump =
        cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_JUMP);
    const bool IsCall =
        cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_CALL);
    if (cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_RET) ||
        cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_IRET) ||
        cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_INT) ||
        (!ReceiverFromRetainResult && Address < SavedReceiver &&
         (IsJump || IsCall)))
      return std::nullopt;
    if (IsJump) {
      const bool BeforeEnter = Address < EnterCall;
      const bool JoinsLockedPath = objcSynchronizedForwardBranch(
          Instruction, BeforeEnter
                           ? EnterCall - (ReceiverFromRetainResult ? 8 : 4)
                           : ExitCall - 4);
      // A pre-acquisition branch can skip the whole lock lifetime and join
      // the verified normal suffix. The source guard remains zero on that
      // path; entering the body, unlock or exceptional pad is still rejected.
      const bool SkipsLock =
          BeforeEnter &&
          objcSynchronizedForwardBranch(Instruction, Landing - 4, ExitCall + 4);
      if (!JoinsLockedPath && !SkipsLock)
        return std::nullopt;
      BypassesLock |= SkipsLock;
    }
    if ((Address != EnterCall && HasCall(Address, "_objc_sync_enter")) ||
        HasCall(Address, "_objc_sync_exit"))
      return std::nullopt;
    if (Address <= EnterCall || !IsCall)
      continue;
    const bool Protected =
        std::any_of(Regions->begin(), Regions->end(), [&](const auto &Region) {
          return Address >= Region.Begin && Address + 4 <= Region.End;
        });
    const auto Hint = objcSynchronizedRuntimeTarget(
        Image, objcSynchronizedBranchTarget(Image, Address));
    const uint8_t ARC = !Hint                                ? 0
                        : Hint->TargetName == "objc_release" ? 1
                        : Hint->TargetName == "objc_retain"  ? 2
                                                             : 0;
    if (Protected)
      ProtectedARC |= ARC;
    else {
      if (!ARC)
        return std::nullopt;
      UnprotectedARC |= ARC;
    }
  }
  // The rendered calls have canonical ARC names, even for register veneers.
  // Only suspend an operation when every occurrence in the locked body is
  // outside the LSDA ranges. Otherwise its source identity is insufficient.
  // The legacy proofs own ordinary single-range lifetimes. This proof also
  // handles a single range when a verified prefix path bypasses the lock;
  // no ARC suspension is needed if every locked-body call is protected.
  if ((Regions->size() == 1 && !BypassesLock && !Prelude) ||
      (Regions->size() > 1 && !UnprotectedARC) ||
      (UnprotectedARC & ProtectedARC))
    return std::nullopt;
  if (!objcSynchronizedNormalSuffixValid(Image, Function.Entry, ExitCall + 4,
                                         Landing, Prelude))
    return std::nullopt;
  ObjCSynchronizedSourceProof Proof{
      EnterCall, ExitCall, ExitCall, Landing,
      objcSynchronizedBranchTarget(Image, Cleanup + 16)};
  Proof.ReceiverIsSavedLocal = ReceiverFromRetainResult;
  Proof.SuspendARC = UnprotectedARC;
  Proof.LandingPadPrelude = Prelude;
  return Proof;
}

// Several acquisitions can share one exceptional cleanup. Prove the lock
// state at every normal CFG join, including the unlocked gap and paths that
// bypass a later acquisition. A protected call must belong to its particular
// lifetime; an unprotected call must run without the cleanup guard. Unlike
// the single-lifetime ARC proof, this deliberately accepts no suspended calls.
inline std::optional<ObjCSynchronizedSourceProof>
proveObjCSynchronizedSequentialCleanup(const BinaryImage &Image,
                                       const HighFunc &Function) {
  const auto Regions = objcSynchronizedInterleavedRanges(Image, Function);
  if (!Regions || Regions->size() < 2 || Function.Entry > InvalidVA - 8)
    return std::nullopt;
  const va_t Landing = Regions->front().Landing;
  const uint8_t Prelude = Regions->front().LandingPadPrelude;
  const va_t Cleanup = Landing + Prelude;
  if (Landing <= Function.Entry ||
      Landing - Function.Entry >
          ::neverd::sdk::
              kObjCSynchronizedSourceProveObjCSynchronizedSequentialCleanupLandingFunctionEntryLimit)
    return std::nullopt;
  const auto Word = [&](va_t Address) {
    return objcSynchronizedWord(Image, Address);
  };
  const auto HasCall = [&](va_t Address, llvm::StringRef Name) {
    return objcSynchronizedCallIs(Image, Address, Name);
  };
  std::vector<ObjCSynchronizedSourceLifetime> Lifetimes;
  for (va_t Address = Function.Entry; Address < Landing; Address += 4) {
    if (HasCall(Address, "_objc_sync_enter")) {
      if (Lifetimes.size() == 16 ||
          (!Lifetimes.empty() && !Lifetimes.back().ExitCall))
        return std::nullopt;
      Lifetimes.push_back({Address, 0});
    } else if (HasCall(Address, "_objc_sync_exit")) {
      if (Lifetimes.empty() || Lifetimes.back().ExitCall)
        return std::nullopt;
      Lifetimes.back().ExitCall = Address;
    } else if (HasCall(Address, "__Unwind_Resume"))
      return std::nullopt;
  }
  if (Lifetimes.size() < 2 || !Lifetimes.back().ExitCall ||
      Lifetimes.front().EnterCall < Function.Entry + 8)
    return std::nullopt;
  const auto SaveX0 = [](unsigned Register) { return 0xaa0003e0U | Register; };
  const auto LoadX0 = [](unsigned Register) {
    return 0xaa0003e0U | (Register << 16);
  };
  unsigned ReceiverRegister = 0, ExceptionRegister = 0;
  for (unsigned Register = 19; Register <= 22; ++Register)
    if (Word(Lifetimes.front().EnterCall - 4) == LoadX0(Register))
      ReceiverRegister = Register;
  for (unsigned Register = 19; Register <= 22; ++Register)
    if (Register != ReceiverRegister && Word(Cleanup) == SaveX0(Register))
      ExceptionRegister = Register;
  if (!ReceiverRegister || !ExceptionRegister ||
      Word(Cleanup + 4) != LoadX0(ReceiverRegister) ||
      !HasCall(Cleanup + 8, "_objc_sync_exit") ||
      Word(Cleanup + 12) != LoadX0(ExceptionRegister) ||
      !HasCall(Cleanup + 16, "__Unwind_Resume"))
    return std::nullopt;
  for (const auto &Lifetime : Lifetimes)
    if (Lifetime.ExitCall <= Lifetime.EnterCall + 4 ||
        Word(Lifetime.EnterCall - 4) != LoadX0(ReceiverRegister) ||
        Word(Lifetime.ExitCall - 4) != LoadX0(ReceiverRegister))
      return std::nullopt;
  va_t SavedReceiver = 0;
  for (va_t Address = Function.Entry; Address < Lifetimes.front().EnterCall - 4;
       Address += 4)
    if (Word(Address) == SaveX0(ReceiverRegister)) {
      if (SavedReceiver || Address - Function.Entry > 32)
        return std::nullopt;
      SavedReceiver = Address;
    }
  if (!SavedReceiver || !objcSynchronizedNormalSuffixValid(
                            Image, Function.Entry,
                            Lifetimes.back().ExitCall + 4, Landing, Prelude))
    return std::nullopt;
  std::vector<unsigned> RegionOwners;
  std::vector<bool> HasRegion(Lifetimes.size(), false);
  for (const auto &Region : *Regions) {
    const auto Owner = std::find_if(
        Lifetimes.begin(), Lifetimes.end(), [&](const auto &Lifetime) {
          return Region.Begin > Lifetime.EnterCall &&
                 Region.End <= Lifetime.ExitCall;
        });
    if (Owner == Lifetimes.end())
      return std::nullopt;
    const unsigned Index = Owner - Lifetimes.begin();
    HasRegion[Index] = true;
    RegionOwners.push_back(Index + 1);
  }
  if (std::find(HasRegion.begin(), HasRegion.end(), false) != HasRegion.end())
    return std::nullopt;

  struct Node {
    std::vector<size_t> Edges;
    unsigned Enter = 0, Exit = 0, Protected = 0;
    bool Call = false, Terminal = false;
  };
  const size_t Count = (Landing - Function.Entry) / 4;
  std::vector<Node> Nodes(Count);
  std::vector<unsigned> Incoming(Count, 0);
  Decoder Decoder;
  if (!Decoder.init(Arch::AArch64))
    return std::nullopt;
  const unsigned ReceiverXRegisters[] = {ARM64_REG_X19, ARM64_REG_X20,
                                         ARM64_REG_X21, ARM64_REG_X22};
  const unsigned ReceiverWRegisters[] = {ARM64_REG_W19, ARM64_REG_W20,
                                         ARM64_REG_W21, ARM64_REG_W22};
  const auto AddEdge = [&](size_t Index, va_t Target) {
    if (Target < Function.Entry || Target >= Landing || Target % 4)
      return false;
    const size_t Next = (Target - Function.Entry) / 4;
    Nodes[Index].Edges.push_back(Next);
    ++Incoming[Next];
    return true;
  };
  for (size_t Index = 0; Index < Count; ++Index) {
    const va_t Address = Function.Entry + Index * 4;
    const uint8_t *Bytes = Image.readVA(Address, 4);
    DecodedInsn Instruction{};
    if (!Bytes || Decoder.decodeOne(Bytes, 4, Address, Instruction) != 4 ||
        !Instruction.Raw ||
        cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_INT) ||
        cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_IRET))
      return std::nullopt;
    cs_regs Reads{}, Writes{};
    uint8_t ReadCount = 0, WriteCount = 0;
    if (cs_regs_access(Decoder.getHandle(), Instruction.Raw, Reads, &ReadCount,
                       Writes, &WriteCount) != CS_ERR_OK)
      return std::nullopt;
    for (uint8_t I = 0; I < WriteCount; ++I)
      if ((Address < SavedReceiver &&
           (Writes[I] == ARM64_REG_X0 || Writes[I] == ARM64_REG_W0)) ||
          (Address > SavedReceiver && Address < Lifetimes.back().ExitCall &&
           (Writes[I] == ReceiverXRegisters[ReceiverRegister - 19] ||
            Writes[I] == ReceiverWRegisters[ReceiverRegister - 19])))
        return std::nullopt;
    auto &Node = Nodes[Index];
    Node.Call =
        cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_CALL);
    for (unsigned I = 0; I < Lifetimes.size(); ++I) {
      if (Address == Lifetimes[I].EnterCall)
        Node.Enter = I + 1;
      if (Address == Lifetimes[I].ExitCall)
        Node.Exit = I + 1;
    }
    for (unsigned I = 0; I < Regions->size(); ++I)
      if (Address >= (*Regions)[I].Begin && Address + 4 <= (*Regions)[I].End)
        Node.Protected = RegionOwners[I];
    const bool Jump =
        cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_JUMP);
    if (Address < SavedReceiver && (Node.Call || Jump))
      return std::nullopt;
    if (Word(Address) == 0xd65f03c0U) {
      Node.Terminal = true;
      continue;
    }
    if (cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_RET))
      return std::nullopt;
    if (Jump && !Node.Call) {
      const auto Target = objcSynchronizedDirectBranchTarget(Instruction);
      if (!Target)
        return std::nullopt;
      const bool Unconditional = branch::A64Branch.matches(*Word(Address));
      if (Unconditional &&
          (*Target < Function.Entry || *Target >= Cleanup + 20)) {
        const auto Name = Image.getFunctionNameAt(*Target);
        if (Name == "_objc_sync_enter" || Name == "_objc_sync_exit" ||
            Name == "__Unwind_Resume")
          return std::nullopt;
        Node.Terminal = true;
        continue;
      }
      if (!AddEdge(Index, *Target))
        return std::nullopt;
      if (Unconditional)
        continue;
    }
    if (!AddEdge(Index, Address + 4))
      return std::nullopt;
  }
  // Acyclic and fully reachable normal code. Merge only equal lock states;
  // a skipped enter, missing exit or cross-lifetime branch cannot inherit it.
  std::vector<size_t> Ready;
  for (size_t I = 0; I < Count; ++I)
    if (!Incoming[I])
      Ready.push_back(I);
  if (Ready.size() != 1 || Ready.front() != 0)
    return std::nullopt;
  std::vector<std::optional<unsigned>> States(Count);
  States[0] = 0;
  for (size_t I = 0; I < Ready.size(); ++I) {
    const size_t Index = Ready[I];
    if (!States[Index])
      return std::nullopt;
    unsigned State = *States[Index];
    const auto &Node = Nodes[Index];
    if (Node.Enter) {
      if (State || Node.Protected)
        return std::nullopt;
      State = Node.Enter;
    } else if (Node.Exit) {
      if (State != Node.Exit || Node.Protected)
        return std::nullopt;
      State = 0;
    } else if (Node.Call && State != Node.Protected)
      return std::nullopt;
    if (Node.Terminal && State)
      return std::nullopt;
    for (const size_t Next : Node.Edges) {
      if (States[Next] && *States[Next] != State)
        return std::nullopt;
      States[Next] = State;
      if (!--Incoming[Next])
        Ready.push_back(Next);
    }
  }
  if (Ready.size() != Count)
    return std::nullopt;
  ObjCSynchronizedSourceProof Proof{
      Lifetimes.front().EnterCall, Lifetimes.back().ExitCall,
      Lifetimes.back().ExitCall, Landing,
      objcSynchronizedBranchTarget(Image, Cleanup + 16)};
  Proof.LandingPadPrelude = Prelude;
  Proof.Lifetimes = std::move(Lifetimes);
  return Proof;
}

// Clang can use a second entry which only saves the exception, while the
// synchronized entry unlocks and branches over that save. Both resume the
// same exception. A resumption-only LSDA range does not own the unlock guard.
inline std::optional<ObjCSynchronizedSourceProof>
proveObjCSynchronizedSplitResumeCleanup(const BinaryImage &Image,
                                        const HighFunc &Function) {
  if (!objcSynchronizedSourceEHValid(Image, Function) ||
      Image.Bits != Bitness::Bits64 || Image.IsRelocatable)
    return std::nullopt;
  const auto &EH = *Function.ExceptionMetadata;
  if (EH.SEH || EH.Cxx || EH.GSCookie || EH.ARMEHABI || EH.Registration ||
      EH.Delphi || EH.DelphiScopes || EH.Go || EH.Rust ||
      EH.ObjC->Runtime != ObjCRuntimeKind::AppleNonFragile ||
      EH.ObjC->UsesFragileSetjmp || EH.ObjC->UsesMSVCTables ||
      (Function.Entry | EH.CodeRange.End) % 4 ||
      EH.Itanium->CallSites.empty() ||
      EH.Itanium->CallSites.size() >
          ::neverd::sdk::
              kObjCSynchronizedSourceProveObjCSynchronizedSplitResumeCleanupEHItaniumCallSitesSizeLimit)
    return std::nullopt;
  va_t Landing = InvalidVA;
  for (const auto &Site : EH.Itanium->CallSites)
    if (Site.LandingPadVA)
      Landing = std::min(Landing, Site.LandingPadVA);
  if (Landing <= Function.Entry || Landing >= EH.CodeRange.End ||
      Landing - Function.Entry >
          ::neverd::sdk::
              kObjCSynchronizedSourceProveObjCSynchronizedSplitResumeCleanupLandingFunctionEntryLimit ||
      Landing % 4)
    return std::nullopt;
  const va_t PadBytes = EH.CodeRange.End - Landing;
  if (PadBytes != 28 && PadBytes != 32 && PadBytes != 36)
    return std::nullopt;
  const bool Forwarded = PadBytes == 36;
  const bool Prelude = PadBytes == 32;
  const va_t Cleanup = Landing + (Forwarded ? 16 : Prelude ? 4 : 0);
  const va_t Passthrough = Forwarded ? Landing : Cleanup + 16;
  const auto Unlocks = [&](va_t Pad) {
    return Pad == Cleanup || (Prelude && Pad == Landing) ||
           (Forwarded && (Pad == Landing + 8 || Pad == Landing + 12));
  };
  const auto Word = [&](va_t Address) {
    return objcSynchronizedWord(Image, Address);
  };
  const auto Target = [&](va_t Address) {
    return objcSynchronizedBranchTarget(Image, Address);
  };
  const auto SyncIs = [&](va_t Address, llvm::StringRef Name) {
    return objcUnwindRuntimeTargetIs(Image, Target(Address), Name,
                                     "/usr/lib/libobjc.A.dylib");
  };
  const va_t ExitTarget = Target(Cleanup + 8);
  const va_t ResumeTarget = Target(EH.CodeRange.End - 4);
  if (!SyncIs(Cleanup + 8, "_objc_sync_exit") ||
      !objcUnwindRuntimeTargetIs(
          Image, ResumeTarget, "__Unwind_Resume",
          "/usr/lib/libSystem.B.dylib|/usr/lib/system/libunwind.dylib") ||
      (!Forwarded && Word(Cleanup + 12) != 0x14000002U) ||
      (Prelude && Word(Landing) != 0x14000001U) ||
      (Forwarded &&
       (Word(Landing + 4) != 0x14000006U || Word(Landing + 8) != 0x14000002U ||
        Word(Landing + 12) != 0x14000001U)))
    return std::nullopt;
  const auto SaveX0 = [](unsigned Register) { return 0xaa0003e0U | Register; };
  const auto LoadX0 = [](unsigned Register) {
    return 0xaa0003e0U | (Register << 16);
  };
  unsigned Receiver = 0, Exception = 0;
  for (unsigned Register = 19; Register <= 22; ++Register) {
    if (Word(Cleanup + 4) == LoadX0(Register))
      Receiver = Register;
    if (Word(Cleanup) == SaveX0(Register))
      Exception = Register;
  }
  if (!Receiver || !Exception || Receiver == Exception ||
      Word(Passthrough) != SaveX0(Exception) ||
      Word(EH.CodeRange.End - 8) != LoadX0(Exception))
    return std::nullopt;
  for (const auto &Call : EH.ObjC->RuntimeCalls)
    if (Call.Kind == ObjCRuntimeCallKind::BeginCatch ||
        Call.Kind == ObjCRuntimeCallKind::EndCatch ||
        Call.Kind == ObjCRuntimeCallKind::Rethrow ||
        Call.Kind == ObjCRuntimeCallKind::FragileTry)
      return std::nullopt;

  va_t Next = Function.Entry;
  size_t Protected = 0, Unlocking = 0, Resuming = 0;
  for (const auto &Site : EH.Itanium->CallSites) {
    const auto &Range = Site.GuardedRange;
    if (Range.Begin < Next || Range.End <= Range.Begin ||
        Range.End > EH.CodeRange.End || (Range.Begin | Range.End) % 4 ||
        Site.FirstActionOffset ||
        (Range.Begin != Next &&
         !objcSynchronizedNonCallGap(Image, Next, Range.Begin)))
      return std::nullopt;
    Next = Range.End;
    if (!Site.LandingPadVA)
      continue;
    const bool Unlock = Unlocks(Site.LandingPadVA);
    if ((!Unlock && Site.LandingPadVA != Passthrough) || Range.End > Landing ||
        std::count_if(EH.ObjC->LandingPads.begin(), EH.ObjC->LandingPads.end(),
                      [&](const auto &Pad) {
                        return Pad.PadVA == Site.LandingPadVA &&
                               Pad.Kind == (Site.LandingPadVA == Cleanup
                                                ? ObjCPadKind::SynchronizedExit
                                                : ObjCPadKind::Cleanup) &&
                               Pad.Catches.empty() &&
                               Pad.GuardedRange.Begin == Range.Begin &&
                               Pad.GuardedRange.End == Range.End;
                      }) != 1)
      return std::nullopt;
    ++Protected;
    Unlock ? ++Unlocking : ++Resuming;
  }
  if (Next != EH.CodeRange.End || !Unlocking || !Resuming ||
      Protected != EH.ObjC->LandingPads.size())
    return std::nullopt;
  std::vector<va_t> Enters, Exits;
  va_t Saved = 0;
  for (va_t Address = Function.Entry; Address < Landing; Address += 4) {
    if (SyncIs(Address, "_objc_sync_enter")) {
      if (Enters.size() == 8)
        return std::nullopt;
      Enters.push_back(Address);
    } else if (SyncIs(Address, "_objc_sync_exit")) {
      if (Exits.size() == 16)
        return std::nullopt;
      Exits.push_back(Address);
    } else if (Target(Address) == ResumeTarget)
      return std::nullopt;
  }
  if (Enters.empty() || Exits.empty())
    return std::nullopt;
  const va_t Enter = Enters.front(), Exit = Exits.back();
  if (Enter < Function.Entry + 8 || Exit <= Enter + 4 ||
      std::any_of(
          Exits.begin(), Exits.end(),
          [&](va_t Address) { return Word(Address - 4) != LoadX0(Receiver); }))
    return std::nullopt;
  const bool RetainedLocal =
      Word(Enter - 4) == SaveX0(Receiver) &&
      objcSynchronizedRuntimeTargetIs(Image, Target(Enter - 8),
                                      "objc_retainAutoreleasedReturnValue",
                                      a64reg::X0);
  if (RetainedLocal)
    Saved = Enter - 4;
  else {
    if (Word(Enter - 4) != LoadX0(Receiver))
      return std::nullopt;
    for (va_t Address = Function.Entry; Address < Enter - 4; Address += 4)
      if (Word(Address) == SaveX0(Receiver)) {
        if (Saved || Address - Function.Entry > 32)
          return std::nullopt;
        Saved = Address;
      }
  }
  if (!Saved)
    return std::nullopt;
  if (RetainedLocal && (Enters.size() != 1 || Exits.size() != 1))
    return std::nullopt;
  for (va_t Address : Enters)
    if (Address != Enter && Word(Address - 4) != LoadX0(Receiver))
      return std::nullopt;
  if (std::any_of(
          Enters.begin(), Enters.end(),
          [&](va_t Address) { return Target(Address) != Target(Enter); }) ||
      std::any_of(Exits.begin(), Exits.end(),
                  [&](va_t Address) { return Target(Address) != ExitTarget; }))
    return std::nullopt;

  struct Node {
    std::vector<size_t> Edges;
    va_t Pad = 0;
    uint8_t ARC = 0;
    bool Call = false, Terminal = false;
    bool WritesReceiver = false;
  };
  const size_t Count = (Landing - Function.Entry) / 4;
  std::vector<Node> Nodes(Count);
  const auto AddEdge = [&](size_t Index, va_t Address) {
    if (Address < Function.Entry || Address >= Landing || Address % 4)
      return false;
    const va_t From = Function.Entry + Index * 4;
    if ((Address <= Saved ||
         std::find(Enters.begin(), Enters.end(), Address) != Enters.end() ||
         std::find(Exits.begin(), Exits.end(), Address) != Exits.end()) &&
        Address != From + 4)
      return false;
    Nodes[Index].Edges.push_back((Address - Function.Entry) / 4);
    return true;
  };
  Decoder Decoder;
  if (!Decoder.init(Arch::AArch64))
    return std::nullopt;
  const unsigned ReceiverX[] = {ARM64_REG_X19, ARM64_REG_X20, ARM64_REG_X21,
                                ARM64_REG_X22};
  const unsigned ReceiverW[] = {ARM64_REG_W19, ARM64_REG_W20, ARM64_REG_W21,
                                ARM64_REG_W22};
  for (size_t Index = 0; Index < Count; ++Index) {
    const va_t Address = Function.Entry + Index * 4;
    const auto *Bytes = Image.readVA(Address, 4);
    DecodedInsn Instruction{};
    if (!Bytes || Decoder.decodeOne(Bytes, 4, Address, Instruction) != 4 ||
        !Instruction.Raw ||
        cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_INT) ||
        cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_IRET))
      return std::nullopt;
    cs_regs Reads{}, Writes{};
    uint8_t ReadCount = 0, WriteCount = 0;
    if (cs_regs_access(Decoder.getHandle(), Instruction.Raw, Reads, &ReadCount,
                       Writes, &WriteCount) != CS_ERR_OK)
      return std::nullopt;
    auto &Node = Nodes[Index];
    for (uint8_t I = 0; I < WriteCount; ++I) {
      if (!RetainedLocal && Address < Saved &&
          (Writes[I] == ARM64_REG_X0 || Writes[I] == ARM64_REG_W0))
        return std::nullopt;
      Node.WritesReceiver |= Writes[I] == ReceiverX[Receiver - 19] ||
                             Writes[I] == ReceiverW[Receiver - 19];
    }
    Node.Call =
        cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_CALL);
    const bool Jump =
        cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_JUMP);
    if (!RetainedLocal && Address < Saved && (Node.Call || Jump))
      return std::nullopt;
    for (const auto &Site : EH.Itanium->CallSites)
      if (Address >= Site.GuardedRange.Begin &&
          Address + 4 <= Site.GuardedRange.End)
        Node.Pad = Site.LandingPadVA;
    if (Node.Call) {
      if (Target(Address) >= Landing && Target(Address) < EH.CodeRange.End)
        return std::nullopt;
      const auto Hint = objcSynchronizedRuntimeTarget(Image, Target(Address));
      if (Hint)
        Node.ARC = Hint->TargetName == "objc_release"  ? 1
                   : Hint->TargetName == "objc_retain" ? 2
                                                       : 0;
      if (objcUnwindNoReturnTarget(Image, Target(Address))) {
        Node.Terminal = true;
        continue;
      }
    }
    if (Word(Address) == 0xd65f03c0U) {
      Node.Terminal = true;
      continue;
    }
    if (cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_RET))
      return std::nullopt;
    if (Jump && !Node.Call) {
      const auto Branch = objcSynchronizedDirectBranchTarget(Instruction);
      if (!Branch)
        return std::nullopt;
      const bool Unconditional = branch::A64Branch.matches(*Word(Address));
      if (Unconditional &&
          (*Branch < Function.Entry || *Branch >= EH.CodeRange.End)) {
        if (objcUnwindRuntimeTargetIs(Image, *Branch, "_objc_sync_enter",
                                      "/usr/lib/libobjc.A.dylib") ||
            objcUnwindRuntimeTargetIs(Image, *Branch, "_objc_sync_exit",
                                      "/usr/lib/libobjc.A.dylib") ||
            *Branch == ResumeTarget)
          return std::nullopt;
        Node.Terminal = true;
        continue;
      }
      if (!AddEdge(Index, *Branch))
        return std::nullopt;
      if (Unconditional)
        continue;
    }
    if (!AddEdge(Index, Address + 4))
      return std::nullopt;
  }
  // Every reachable join must have the same lock state. This also handles a
  // loop which preserves it: no path can inherit a skipped enter or exit.
  struct State {
    bool Held = false, ReceiverKnown = false;
  };
  std::vector<std::optional<State>> States(Count);
  std::vector<size_t> Ready{0};
  States[0] = State{};
  uint8_t SuspendARC = 0, ProtectedARC = 0;
  for (size_t I = 0; I < Ready.size(); ++I) {
    const size_t Index = Ready[I];
    const va_t Address = Function.Entry + Index * 4;
    const auto &Node = Nodes[Index];
    auto Current = *States[Index];
    bool &Held = Current.Held;
    if (Address == Saved)
      Current.ReceiverKnown = true;
    else if (Node.WritesReceiver) {
      if (Held)
        return std::nullopt;
      Current.ReceiverKnown = false;
    }
    if (std::find(Enters.begin(), Enters.end(), Address) != Enters.end()) {
      if (Held || Node.Pad || !Current.ReceiverKnown)
        return std::nullopt;
      Held = true;
    } else if (std::find(Exits.begin(), Exits.end(), Address) != Exits.end()) {
      if (!Held || Node.Pad || !Current.ReceiverKnown)
        return std::nullopt;
      Held = false;
    } else if (Node.Call && !Node.Terminal) {
      if (Unlocks(Node.Pad)) {
        if (!Held)
          return std::nullopt;
        ProtectedARC |= Node.ARC;
      } else if (Held) {
        if (!Node.ARC)
          return std::nullopt;
        SuspendARC |= Node.ARC;
      }
    }
    if (Node.Terminal && Held)
      return std::nullopt;
    for (const auto Successor : Node.Edges) {
      if (States[Successor]) {
        if (States[Successor]->Held != Held)
          return std::nullopt;
        if (States[Successor]->ReceiverKnown && !Current.ReceiverKnown) {
          States[Successor]->ReceiverKnown = false;
          Ready.push_back(Successor);
        }
      } else {
        States[Successor] = Current;
        Ready.push_back(Successor);
      }
    }
  }
  if (std::any_of(States.begin(), States.end(),
                  [](const auto &S) { return !S; }) ||
      (SuspendARC & ProtectedARC))
    return std::nullopt;
  ObjCSynchronizedSourceProof Proof{Enter, Exit, Exit, Landing, ResumeTarget};
  Proof.SuspendARC = SuspendARC;
  Proof.SplitResumeTail = true;
  Proof.LandingPadExitTarget = ExitTarget;
  Proof.SplitResumePadBytes = static_cast<uint8_t>(PadBytes);
  Proof.ReceiverIsSavedLocal = RetainedLocal;
  if (Enters.size() != 1 || Exits.size() != 1) {
    Proof.NormalEnterCalls = std::move(Enters);
    Proof.NormalExitCalls = std::move(Exits);
    Proof.NormalEnterTarget = Target(Enter);
  }
  return Proof;
}

// The private self cell can survive several LSDA ranges separated by runtime
// calls that deliberately have no cleanup. Authenticate those operations and
// suspend their source guard without asserting that they cannot throw.
inline std::optional<ObjCSynchronizedSourceProof>
proveObjCSynchronizedFrameCleanup(const BinaryImage &Image,
                                  const HighFunc &Function) {
  const auto Regions = objcSynchronizedInterleavedRanges(Image, Function);
  if (!Regions || Regions->front().LandingPadPrelude ||
      Function.Entry > InvalidVA - 32)
    return std::nullopt;
  const va_t Entry = Function.Entry, Enter = Entry + 28;
  const va_t Landing = Regions->front().Landing;
  if (Regions->front().Begin != Enter + 4 || Landing < Entry + 64)
    return std::nullopt;
  const auto Word = [&](va_t Address) {
    return objcSynchronizedWord(Image, Address);
  };
  const auto Target = [&](va_t Address) {
    return objcSynchronizedBranchTarget(Image, Address);
  };
  const auto Sync = [&](va_t Address, llvm::StringRef Name) {
    return objcUnwindRuntimeTargetIs(Image, Target(Address), Name,
                                     "/usr/lib/libobjc.A.dylib");
  };
  const va_t Exit = Landing - 24;
  const auto Frame = proveObjCSynchronizedPrivateFrame(Image, Entry, Exit);
  if (!Frame || Regions->back().End > Exit - 4 ||
      Word(Entry + 24) != 0xf85e83a0U ||
      !objcSynchronizedRuntimeTargetIs(Image, Target(Entry + 20), "objc_retain",
                                       a64reg::X0) ||
      !Sync(Enter, "_objc_sync_enter") || Word(Exit - 4) != 0xf85e83a0U ||
      !Sync(Exit, "_objc_sync_exit") || Word(Exit + 4) != 0xf85e83a0U ||
      Word(Exit + 8) != (0xa9407bfdU | ((Frame->Bytes - 16) / 8 << 15)) ||
      Word(Exit + 12) != (0xa9404ff4U | ((Frame->Bytes - 32) / 8 << 15)) ||
      Word(Exit + 16) != (0x910003ffU | (Frame->Bytes << 10)) ||
      Word(Landing) != 0xaa0003f3U || Word(Landing + 4) != 0xf85e83a0U ||
      !Sync(Landing + 8, "_objc_sync_exit") ||
      Target(Exit) != Target(Landing + 8) ||
      Word(Landing + 12) != 0xaa1303e0U ||
      !objcUnwindRuntimeTargetIs(Image, Target(Landing + 16), "__Unwind_Resume",
                                 "/usr/lib/libSystem.B.dylib|/usr/lib/system/"
                                 "libunwind.dylib"))
    return std::nullopt;
  const auto Tail = Word(Exit + 20);
  if (!Tail || !branch::A64Branch.matches(*Tail) ||
      !objcSynchronizedRuntimeTargetIs(
          Image, branch::a64BranchTarget(*Tail, Exit + 20).value_or(0),
          "objc_release", a64reg::X0))
    return std::nullopt;
  Decoder Decoder;
  if (!Decoder.init(Arch::AArch64))
    return std::nullopt;
  uint8_t ProtectedCalls = 0, UnprotectedCalls = 0;
  for (va_t Address = Entry + 20; Address < Exit; Address += 4) {
    const auto *Bytes = Image.readVA(Address, 4);
    DecodedInsn Instruction{};
    if (!Bytes || Decoder.decodeOne(Bytes, 4, Address, Instruction) != 4 ||
        !Instruction.Raw)
      return std::nullopt;
    if (cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_RET) ||
        cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_INT) ||
        cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_IRET))
      return std::nullopt;
    if (cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_JUMP) &&
        (Address <= Enter ||
         !objcSynchronizedForwardBranch(Instruction, Exit - 4, Enter + 4)))
      return std::nullopt;
    if (!cs_insn_group(Decoder.getHandle(), Instruction.Raw, CS_GRP_CALL))
      continue;
    if (!Target(Address))
      return std::nullopt;
    if (Address <= Enter)
      continue;
    if (objcSynchronizedCallIs(Image, Address, "_objc_sync_enter") ||
        objcSynchronizedCallIs(Image, Address, "_objc_sync_exit"))
      return std::nullopt;
    const auto Runtime = objcSynchronizedRuntimeTarget(Image, Target(Address));
    uint8_t Operation = Runtime && Runtime->TargetName == "objc_release"  ? 1
                        : Runtime && Runtime->TargetName == "objc_retain" ? 2
                                                                          : 0;
    if (objcUnwindRuntimeTargetIs(Image, Target(Address),
                                  "_dispatch_group_leave",
                                  "/usr/lib/libSystem.B.dylib|/usr/lib/system/"
                                  "libdispatch.dylib"))
      Operation = 4;
    const bool Protected =
        std::any_of(Regions->begin(), Regions->end(), [&](const auto &Region) {
          return Address >= Region.Begin && Address + 4 <= Region.End;
        });
    if (Protected)
      ProtectedCalls |= Operation;
    else {
      if (!Operation)
        return std::nullopt;
      UnprotectedCalls |= Operation;
    }
  }
  if (ProtectedCalls & UnprotectedCalls)
    return std::nullopt;
  ObjCSynchronizedSourceProof Proof{Enter, Exit, Exit, Landing,
                                    Target(Landing + 16)};
  Proof.PrivateFrame = Frame;
  Proof.NormalEnterTarget = Target(Enter);
  Proof.LandingPadExitTarget = Target(Exit);
  Proof.SuspendARC = UnprotectedCalls & 3;
  Proof.SuspendDispatchLeave = UnprotectedCalls & 4;
  return Proof;
}

inline std::optional<ObjCSynchronizedSourceProof>
proveObjCSynchronizedReceiverCleanup(const BinaryImage &Image,
                                     const HighFunc &Function) {
  if (auto Proof =
          proveObjCSynchronizedRetainedStackReceiverCleanup(Image, Function))
    return Proof;
  if (auto Proof = proveObjCSynchronizedStackReceiverCleanup(Image, Function))
    return Proof;
  if (auto Proof =
          proveObjCSynchronizedRegisterReceiverCleanup(Image, Function))
    return Proof;
  if (auto Proof =
          proveObjCSynchronizedBranchedLocalReceiverCleanup(Image, Function))
    return Proof;
  if (auto Proof = proveObjCSynchronizedInterleavedCleanup(Image, Function))
    return Proof;
  if (auto Proof = proveObjCSynchronizedSequentialCleanup(Image, Function))
    return Proof;
  if (auto Proof = proveObjCSynchronizedFrameCleanup(Image, Function))
    return Proof;
  return proveObjCSynchronizedSplitResumeCleanup(Image, Function);
}

// The proved pad is an exceptional entry, not a normal source path. A full
// image analysis can retain it as an unreachable suffix after the ordinary
// return. Remove that suffix only when its label and statement addresses
// match the proved branch entry and five cleanup instructions, with no source
// edge into either exceptional entry.
// The cleanup variable below supplies the same exceptional unlock.
inline bool omitProvenObjCSynchronizedSplitResumeTail(
    HighFunc &Function, const ObjCSynchronizedSourceProof &Proof) {
  if (!Proof.SplitResumeTail || !Proof.LandingPad || !Proof.ResumeTarget ||
      !Proof.LandingPadExitTarget || Proof.LandingPadPrelude ||
      (Proof.SplitResumePadBytes != 28 && Proof.SplitResumePadBytes != 32 &&
       Proof.SplitResumePadBytes != 36) ||
      Proof.LandingPad > InvalidVA - Proof.SplitResumePadBytes ||
      !Function.ExceptionMetadata ||
      Function.ExceptionMetadata->CodeRange.Begin != Function.Entry ||
      Function.ExceptionMetadata->CodeRange.End !=
          Proof.LandingPad + Proof.SplitResumePadBytes ||
      Function.Body.empty())
    return false;
  const auto InPad = [&](va_t Address) {
    return Address >= Proof.LandingPad &&
           Address < Proof.LandingPad + Proof.SplitResumePadBytes;
  };
  const bool Forwarded = Proof.SplitResumePadBytes == 36;
  const bool Prelude = Proof.SplitResumePadBytes == 32;
  const va_t Cleanup = Proof.LandingPad + (Forwarded ? 16 : Prelude ? 4 : 0);
  const va_t Passthrough = Forwarded ? Proof.LandingPad : Cleanup + 16;
  const auto UnlockAlias = [&](va_t Address) {
    return (Prelude && Address == Proof.LandingPad) ||
           (Forwarded && (Address == Proof.LandingPad + 8 ||
                          Address == Proof.LandingPad + 12));
  };
  auto &Body = Function.Body;
  const auto Marker = std::find_if(
      Body.begin(), Body.end(), [&](const auto &S) { return InPad(S.Addr); });
  std::vector<HighStmt *> Pending;
  for (auto It = Body.begin(); It != Marker; ++It)
    Pending.push_back(&*It);
  size_t Budget = kSourceValueWork;
  std::optional<MedVar> SavedReceiver;
  std::vector<va_t> NormalCalls;
  while (!Pending.empty()) {
    if (!Budget--)
      return false;
    auto *S = Pending.back();
    Pending.pop_back();
    if (InPad(S->Addr) || (S->Kind == StmtKind::Goto && InPad(S->GotoTarget)))
      return false;
    const bool NormalEnter =
        std::find(Proof.NormalEnterCalls.begin(), Proof.NormalEnterCalls.end(),
                  S->Addr) != Proof.NormalEnterCalls.end();
    const bool NormalExit =
        std::find(Proof.NormalExitCalls.begin(), Proof.NormalExitCalls.end(),
                  S->Addr) != Proof.NormalExitCalls.end();
    if (NormalEnter || NormalExit) {
      const auto Call = objcUnwindStatementCall(*S);
      if (!Call || Call->Kind != ExprKind::Call || Call->IsIndirectCall ||
          Call->IndirectTarget || Call->Operands.size() != 1 ||
          Call->CallAddr != (NormalEnter ? Proof.NormalEnterTarget
                                         : Proof.LandingPadExitTarget) ||
          Function.Params.empty() || !Function.Params[0].Type ||
          Function.Params[0].Type->Kind != NdTypeKind::Ptr ||
          Function.Params[0].Type->Size != 8 ||
          std::find(NormalCalls.begin(), NormalCalls.end(), S->Addr) !=
              NormalCalls.end())
        return false;
      size_t Budget = kSourceSliceWork;
      const auto Receiver =
          objcUnwindScalarSlice(Call->Operands[0], {}, false, Budget);
      if (!Receiver || Receiver->Offset || Receiver->Bytes != 8)
        return false;
      // Preserve the native pipeline's shared expressions. Only the source
      // copy receives the exact formal pointer reconstructed by those bytes.
      auto Copy = std::make_shared<HighExpr>(*Call);
      Copy->Operands = {
          HighExpr::makeVar(Receiver->Root, Function.Params[0].Type)};
      if (S->Kind == StmtKind::Call)
        S->CallExpr = std::move(Copy);
      else
        S->Val = std::move(Copy);
      NormalCalls.push_back(S->Addr);
    }
    if (Proof.ReceiverIsSavedLocal && S->Addr == Proof.ExitCall) {
      const auto &Call = S->Val;
      if (!Call || Call->Kind != ExprKind::Call || Call->IsIndirectCall ||
          Call->CallAddr != Proof.LandingPadExitTarget ||
          Call->Operands.size() != 1 || !Call->Operands[0] ||
          Call->Operands[0]->Kind != ExprKind::Var ||
          !Call->Operands[0]->Operands.empty() ||
          Call->Operands[0]->Var.Size != 8 ||
          (Call->Operands[0]->Var.Kind != MedVar::Temp &&
           Call->Operands[0]->Var.Kind != MedVar::Reg))
        return false;
      const auto &Receiver = Call->Operands[0]->Var;
      if (SavedReceiver && *SavedReceiver != Receiver)
        return false;
      SavedReceiver = Receiver;
    }
    for (auto &Child : S->Body)
      Pending.push_back(&Child);
    for (auto &Child : S->ElseBody)
      Pending.push_back(&Child);
    for (auto &Case : S->Cases)
      for (auto &Child : Case.Body)
        Pending.push_back(&Child);
    for (auto &Child : S->DefaultBody)
      Pending.push_back(&Child);
    for (auto &Clause : S->EHClauseBodies)
      for (auto &Child : Clause)
        Pending.push_back(&Child);
  }
  if (Proof.ReceiverIsSavedLocal && !SavedReceiver)
    return false;
  if (NormalCalls.size() !=
      Proof.NormalEnterCalls.size() + Proof.NormalExitCalls.size())
    return false;
  if (Marker == Body.end())
    return Body.back().Kind == StmtKind::Return;
  if (Marker == Body.begin() || Marker->Addr != Proof.LandingPad)
    return false;
  ObjCUnwindScalarCopies Exceptions;
  const auto Variable = [](const ExprPtr &E) {
    return E && E->Kind == ExprKind::Var && E->Operands.empty() &&
           E->MemoryOrdering == NdMemoryOrdering::None &&
           E->MemoryAddressSpace == NdMemoryAddressSpace::Default;
  };
  const auto Exception = [&](const ExprPtr &E) {
    size_t Budget = kSourceSliceWork;
    const auto Slice = objcUnwindScalarSlice(E, Exceptions, true, Budget);
    return Slice && !Slice->Offset && Slice->Bytes == 8;
  };
  std::vector<va_t> Exits;
  bool SawResume = false;
  for (auto It = Marker; It != Body.end(); ++It) {
    const auto &S = *It;
    if (!S.Body.empty() || !S.ElseBody.empty() || !S.Cases.empty() ||
        !S.DefaultBody.empty() || !S.EHClauseBodies.empty() ||
        !S.EHClauses.empty() || S.MemoryOrdering != NdMemoryOrdering::None ||
        S.MemoryAddressSpace != NdMemoryAddressSpace::Default || S.Cond ||
        S.StoreAddr || S.StoreVal || S.SwitchExpr ||
        (S.CallExpr && S.Kind != StmtKind::Call) ||
        (S.Kind == StmtKind::Call && (S.Dst || S.Val)) || S.EHRange.Begin ||
        S.EHRange.End)
      return false;
    if (It + 1 == Body.end() && !S.Addr && S.Kind == StmtKind::Return &&
        !S.Dst && !S.Val && !S.GotoTarget &&
        (!S.RetVal ||
         (S.RetVal->Operands.empty() && (S.RetVal->Kind == ExprKind::Var ||
                                         S.RetVal->Kind == ExprKind::Undef))))
      continue;
    if (!InPad(S.Addr) || S.RetVal)
      return false;
    if (S.Kind == StmtKind::Block && !S.Dst && !S.Val && !S.GotoTarget)
      continue;
    if (S.Kind == StmtKind::Goto && !S.Dst && !S.Val && InPad(S.GotoTarget))
      continue;
    if ((S.Kind != StmtKind::Assign && S.Kind != StmtKind::ExprStmt &&
         S.Kind != StmtKind::Call) ||
        S.GotoTarget || (S.Dst && !Variable(S.Dst)))
      return false;
    if (S.Kind == StmtKind::Assign && Variable(S.Dst) &&
        (S.Dst->Var.Kind == MedVar::Temp || S.Dst->Var.Kind == MedVar::Reg) &&
        S.Dst->Var.Size <= 8) {
      size_t Budget = kSourceSliceWork;
      const auto Slice = objcUnwindScalarSlice(S.Val, Exceptions, true, Budget);
      if (Slice && Slice->Bytes == S.Dst->Var.Size) {
        std::erase_if(Exceptions, [&](const auto &Copy) {
          return Copy.first == S.Dst->Var;
        });
        Exceptions.emplace_back(S.Dst->Var, *Slice);
        continue;
      }
    }
    const auto Call = objcUnwindStatementCall(S);
    if (!Call || Call->Kind != ExprKind::Call || Call->IsIndirectCall ||
        Call->IndirectTarget || Call->IntrinsicId != Intrinsic::None ||
        Call->MemoryOrdering != NdMemoryOrdering::None ||
        Call->MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        Call->Operands.size() != 1)
      return false;
    const auto &Argument = Call->Operands.front();
    size_t ReceiverBudget = kSourceSliceWork;
    const auto Receiver =
        objcUnwindScalarSlice(Argument, {}, false, ReceiverBudget);
    if (Call->CallAddr == Proof.LandingPadExitTarget &&
        (S.Addr == Cleanup + 8 || UnlockAlias(S.Addr)) &&
        (SavedReceiver
             ? Variable(Argument) && Argument->Var.Size == 8 &&
                   Argument->Var == *SavedReceiver
             : Receiver && !Receiver->Offset && Receiver->Bytes == 8) &&
        std::find(Exits.begin(), Exits.end(), S.Addr) == Exits.end()) {
      Exits.push_back(S.Addr);
      continue;
    }
    if (Call->CallAddr != Proof.ResumeTarget || !Exception(Argument) ||
        (S.Addr != Proof.LandingPad + Proof.SplitResumePadBytes - 4 &&
         (Forwarded ? S.Addr != Passthrough + 4 && !UnlockAlias(S.Addr)
                    : S.Addr != Cleanup + 12)))
      return false;
    SawResume = true;
  }
  if (Exits.empty() || !SawResume)
    return false;
  Body.erase(Marker, Body.end());
  return true;
}

inline bool
omitProvenObjCSynchronizedLandingPad(HighFunc &Function,
                                     const ObjCSynchronizedSourceProof &Proof) {
  if (Proof.SplitResumeTail)
    return omitProvenObjCSynchronizedSplitResumeTail(Function, Proof);
  if (!Proof.LandingPad || !Proof.ResumeTarget ||
      (Proof.LandingPadPrelude != 0 && Proof.LandingPadPrelude != 4) ||
      Proof.LandingPad > InvalidVA - 20 - Proof.LandingPadPrelude ||
      Function.Body.empty())
    return false;
  const va_t Landing = Proof.LandingPad;
  const va_t Cleanup = Landing + Proof.LandingPadPrelude;
  const auto InPad = [&](va_t Address) {
    return Address >= Landing && Address < Cleanup + 20;
  };
  const auto EmptyLabel = [](const HighStmt &S) {
    return S.Kind == StmtKind::Block && S.Body.empty() && S.ElseBody.empty() &&
           !S.Dst && !S.Val && !S.Cond && !S.RetVal && !S.StoreAddr &&
           !S.StoreVal && !S.CallExpr && !S.SwitchExpr && S.Cases.empty() &&
           S.DefaultBody.empty() && !S.GotoTarget && S.EHClauseBodies.empty() &&
           S.EHClauses.empty() && S.MemoryOrdering == NdMemoryOrdering::None &&
           S.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
           !S.EHRange.Begin && !S.EHRange.End;
  };
  std::vector<std::pair<const HighStmt *, bool>> Pending;
  for (const auto &Statement : Function.Body)
    Pending.emplace_back(&Statement, false);
  size_t Budget = kSourceValueWork;
  while (!Pending.empty()) {
    if (!Budget--)
      return false;
    const auto [Statement, Nested] = Pending.back();
    Pending.pop_back();
    if (Nested && InPad(Statement->Addr))
      return false;
    if (Statement->Kind == StmtKind::Goto && InPad(Statement->GotoTarget))
      return false;
    for (const auto &Child : Statement->Body)
      Pending.emplace_back(&Child, true);
    for (const auto &Child : Statement->ElseBody)
      Pending.emplace_back(&Child, true);
    for (const auto &Case : Statement->Cases)
      for (const auto &Child : Case.Body)
        Pending.emplace_back(&Child, true);
    for (const auto &Child : Statement->DefaultBody)
      Pending.emplace_back(&Child, true);
    for (const auto &Clause : Statement->EHClauseBodies)
      for (const auto &Child : Clause)
        Pending.emplace_back(&Child, true);
  }
  auto &Body = Function.Body;
  const auto Marker = std::find_if(
      Body.begin(), Body.end(), [&](const auto &S) { return InPad(S.Addr); });
  if (Marker == Body.end())
    return !Proof.PrivateFrame && Body.back().Kind == StmtKind::Return;
  if (Marker == Body.begin() || (Marker - 1)->Kind != StmtKind::Return ||
      Marker->Addr != Landing || !EmptyLabel(*Marker))
    return false;
  bool SawResume = false;
  for (auto It = Marker + 1; It != Body.end(); ++It) {
    if (Proof.LandingPadPrelude && It == Marker + 1 && It->Addr == Cleanup &&
        EmptyLabel(*It))
      continue;
    const auto Call = objcUnwindStatementCall(*It);
    if (It->Addr == Cleanup + 16 && Call && Call->Kind == ExprKind::Call &&
        Call->CallAddr == Proof.ResumeTarget) {
      if (Proof.PrivateFrame) {
        size_t Budget = kSourceSliceWork;
        const auto Exception =
            Call->Operands.size() == 1
                ? objcUnwindScalarSlice(Call->Operands[0], {}, true, Budget)
                : std::nullopt;
        if (Call->IsIndirectCall || Call->IndirectTarget || !Exception ||
            Exception->Offset || Exception->Bytes != 8)
          return false;
      }
      SawResume = true;
    } else if (InPad(It->Addr) && It->Kind == StmtKind::Assign &&
               (It->Addr == Cleanup + 4 || It->Addr == Cleanup + 8)) {
      if (Proof.PrivateFrame && It->Addr == Cleanup + 8 &&
          (!Call || Call->Kind != ExprKind::Call || Call->IsIndirectCall ||
           Call->IndirectTarget ||
           Call->CallAddr != Proof.LandingPadExitTarget ||
           Call->Operands.size() != 1))
        return false;
      continue;
    } else if (It + 1 == Body.end() && It->Addr == 0 &&
               It->Kind == StmtKind::Return &&
               (!It->RetVal || (It->RetVal->Operands.empty() &&
                                (It->RetVal->Kind == ExprKind::Var ||
                                 It->RetVal->Kind == ExprKind::Undef))))
      continue;
    else
      return false;
    if (!It->Body.empty() || !It->ElseBody.empty() || !It->Cases.empty() ||
        !It->DefaultBody.empty() || !It->EHClauseBodies.empty())
      return false;
  }
  if (!SawResume)
    return false;
  if (Proof.PrivateFrame && !projectObjCSynchronizedFrameReceiver(
                                Function, *Proof.PrivateFrame, Proof.EnterCall,
                                Proof.ExitCall, Proof.NormalEnterTarget,
                                Proof.LandingPadExitTarget, Proof.LandingPad))
    return false;
  Body.erase(Marker, Body.end());
  return true;
}

inline std::optional<std::string>
objcSynchronizedSavedLocalArgument(llvm::StringRef Source, size_t Call,
                                   llvm::StringRef Name) {
  if (Call == std::string::npos)
    return std::nullopt;
  const size_t Begin = Call + Name.size();
  unsigned Depth = 0;
  size_t End = Begin;
  for (
      ;
      End < Source.size() &&
      End - Begin <
          ::neverd::sdk::
              kObjCSynchronizedSourceObjcSynchronizedSavedLocalArgumentEndBeginLimit;
      ++End) {
    const char Character = Source[End];
    if (Character == '(')
      ++Depth;
    else if (Character == ')') {
      if (!Depth)
        break;
      --Depth;
    } else if (Character == '\n' || Character == ';' || Character == ',' ||
               Character == '"' || Character == '\'')
      return std::nullopt;
  }
  if (End == Source.size() ||
      End - Begin >= ::neverd::sdk::kMaxSynchronizedSourceTokenBytes)
    return std::nullopt;
  const llvm::StringRef Argument = Source.slice(Begin, End);
  const llvm::StringRef Prefix = "(void*)(uintptr_t)(";
  if (!Argument.starts_with(Prefix) || !Argument.ends_with(')'))
    return std::nullopt;
  llvm::StringRef NameOnly = Argument.drop_front(Prefix.size()).drop_back();
  // Private-frame forwarding restores a load's signedness with an exact
  // 64-bit integer view. Both views preserve the saved pointer word. Return
  // the canonical argument for identity checks and the guard; the emitted
  // runtime calls themselves keep their original casts.
  if (!NameOnly.consume_front("(int64_t)"))
    NameOnly.consume_front("(uint64_t)");
  const auto Alpha = [](char C) {
    return (C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') || C == '_';
  };
  const auto Digit = [](char C) { return C >= '0' && C <= '9'; };
  if (NameOnly.empty() || NameOnly == "objc_self" || !Alpha(NameOnly.front()))
    return std::nullopt;
  for (char Character : NameOnly)
    if (!Alpha(Character) && !Digit(Character))
      return std::nullopt;
  return (Prefix + NameOnly + ")").str();
}

// Locate an emitted definition, ignoring prototypes, comments, and braces in
// literals. An empty name selects the first Objective-C method, for existing
// single-method projections; an exact name scopes a dependency's cleanup.
inline std::optional<std::pair<size_t, size_t>>
objcSynchronizedSourceBody(llvm::StringRef Source,
                           llvm::StringRef FunctionName = {}) {
  enum class Lexical { Normal, String, Character, LineComment, BlockComment };
  Lexical State = Lexical::Normal;
  unsigned Braces = 0, Parameters = 0;
  bool Candidate = false;
  size_t Open = std::string::npos;
  const auto Identifier = [](char C) {
    return (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') ||
           (C >= '0' && C <= '9') || C == '_';
  };
  const size_t Limit = std::min<size_t>(
      Source.size(),
      ::neverd::sdk::
          kObjCSynchronizedSourceObjcSynchronizedSourceBodyLimitMinArgument2);
  for (size_t I = 0; I < Limit; ++I) {
    const char C = Source[I];
    const char Next = I + 1 < Limit ? Source[I + 1] : 0;
    if (State == Lexical::String || State == Lexical::Character) {
      if (C == '\\')
        ++I;
      else if ((State == Lexical::String && C == '"') ||
               (State == Lexical::Character && C == '\''))
        State = Lexical::Normal;
      continue;
    }
    if (State == Lexical::LineComment) {
      if (C == '\n')
        State = Lexical::Normal;
      continue;
    }
    if (State == Lexical::BlockComment) {
      if (C == '*' && Next == '/') {
        State = Lexical::Normal;
        ++I;
      }
      continue;
    }
    if (C == '"' || C == '\'') {
      State = C == '"' ? Lexical::String : Lexical::Character;
      continue;
    }
    if (C == '/' && (Next == '/' || Next == '*')) {
      State = Next == '/' ? Lexical::LineComment : Lexical::BlockComment;
      ++I;
      continue;
    }
    if (Open == std::string::npos && !Braces && !Candidate && Identifier(C)) {
      size_t End = I + 1;
      while (End < Limit && Identifier(Source[End]))
        ++End;
      const auto Name = Source.slice(I, End);
      if (FunctionName.empty() ? Name.starts_with("neverd_objc_imp_")
                               : Name == FunctionName) {
        size_t NextToken = End;
        while (NextToken < Limit &&
               (Source[NextToken] == ' ' || Source[NextToken] == '\t' ||
                Source[NextToken] == '\n'))
          ++NextToken;
        Candidate = NextToken < Limit && Source[NextToken] == '(';
        Parameters = 0;
      }
      I = End - 1;
      continue;
    }
    if (Candidate) {
      if (C == '(')
        ++Parameters;
      else if (C == ')') {
        if (!Parameters)
          Candidate = false;
        else
          --Parameters;
      } else if (C == ';' && !Parameters)
        Candidate = false;
      else if (C == '{' && !Parameters) {
        Open = I;
        Candidate = false;
      }
    }
    if (C == '{')
      ++Braces;
    else if (C == '}') {
      if (!Braces)
        return std::nullopt;
      if (!--Braces && Open != std::string::npos)
        return std::pair{Open, I + 1};
    }
  }
  return std::nullopt;
}

// The emitter has already rendered and checked the method. This constrained
// edit adds only the exceptional unlock around the rendered runtime calls;
// every other statement retains its existing source binding.
inline bool objcSynchronizedSelfArgument(llvm::StringRef Source, size_t At,
                                         llvm::StringRef Name) {
  if (At == std::string::npos || At + Name.size() > Source.size())
    return false;
  for (llvm::StringRef Argument :
       {"objc_self)", "(void*)(uintptr_t)(objc_self))",
        "(void*)(uintptr_t)((uintptr_t)objc_self))"})
    if (Source.drop_front(At + Name.size()).starts_with(Argument))
      return true;
  return false;
}

inline std::optional<std::string>
objcSynchronizedSuspendRuntimeCalls(llvm::StringRef Source, size_t Begin,
                                    size_t End, uint8_t Operations,
                                    llvm::StringRef Receiver) {
  if (!Operations || Operations > 7 || Begin >= End || End > Source.size())
    return std::nullopt;
  std::vector<std::pair<size_t, std::string>> Calls;
  std::string Helpers;
  for (const auto &[Operation, Name] :
       {std::pair<uint8_t, llvm::StringRef>{1, "objc_release"},
        {2, "objc_retain"},
        {4, "neverd_darwin_dispatch_group_leave"}}) {
    if (!(Operations & Operation))
      continue;
    const std::string Call = (Name + "(").str();
    size_t Count = 0;
    for (size_t At = Source.find(Call, Begin);
         At != std::string::npos && At < End;
         At = Source.find(Call, At + Call.size())) {
      if (++Count >
          ::neverd::sdk::
              kObjCSynchronizedSourceObjcSynchronizedSuspendRuntimeCallsCountLimit)
        return std::nullopt;
      const size_t Line = Source.rfind('\n', At);
      if (Line == std::string::npos)
        return std::nullopt;
      auto Prefix = Source.slice(Line + 1, At).trim();
      if (Operation != 2) {
        if (!Prefix.empty())
          return std::nullopt;
      } else {
        const size_t Assign = Prefix.find('=');
        if (Assign != std::string::npos) {
          const auto Local = Prefix.take_front(Assign).trim();
          if (Local.empty() || llvm::isDigit(Local.front()) ||
              !std::all_of(Local.begin(), Local.end(),
                           [](char C) { return llvm::isAlnum(C) || C == '_'; }))
            return std::nullopt;
          Prefix = Prefix.drop_front(Assign + 1).trim();
        }
        if (Prefix != "(uint64_t)(" && Prefix != "(uint64_t)(uintptr_t)(" &&
            Prefix != "(uint64_t)(uintptr_t)")
          return std::nullopt;
      }
      const auto Suffix = Operation == 4
                              ? llvm::StringRef("dispatch_group_leave")
                              : Name.drop_front(5);
      Calls.emplace_back(At, ("neverd_objc_sync_" + Suffix +
                              "(&neverd_objc_sync_guard, " + Receiver + ", ")
                                 .str());
    }
    if (!Count)
      return std::nullopt;
    std::string Helper;
    if (Operation == 1)
      Helper = "extern void objc_release(void*);\n"
               "static void neverd_objc_sync_release(void **guard, void *lock, "
               "void *object) {\n"
               "    void *active = *guard ? lock : 0;\n"
               "    *guard = 0;\n"
               "    objc_release(object);\n"
               "    *guard = active;\n"
               "}\n";
    else if (Operation == 2)
      Helper = "extern void *objc_retain(void*);\n"
               "static void *neverd_objc_sync_retain(void **guard, void *lock, "
               "void *object) {\n"
               "    void *active = *guard ? lock : 0;\n"
               "    *guard = 0;\n"
               "    void *result = objc_retain(object);\n"
               "    *guard = active;\n"
               "    return result;\n"
               "}\n";
    else
      Helper =
          "extern void neverd_darwin_dispatch_group_leave(void*) "
          "__asm__(\"_dispatch_group_leave\");\n"
          "static void neverd_objc_sync_dispatch_group_leave(void **guard, "
          "void *lock, void *group) {\n"
          "    void *active = *guard ? lock : 0;\n"
          "    *guard = 0;\n"
          "    neverd_darwin_dispatch_group_leave(group);\n"
          "    *guard = active;\n"
          "}\n";
    if (!Source.contains(Helper))
      Helpers += Helper;
  }
  std::sort(Calls.begin(), Calls.end(),
            [](const auto &A, const auto &B) { return A.first > B.first; });
  std::string Result = Source.str();
  for (const auto &[At, Replacement] : Calls) {
    const size_t Length = Source.find('(', At) - At + 1;
    Result.replace(At, Length, Replacement);
  }
  return Helpers + Result;
}

// The emitter may expand a shared unlock into early-return tails. Before
// accepting another rendered unlock, require the preceding one to terminate
// its path with an unconditional return. Ignore literals and comments; reject
// intervening control flow, labels and nested scopes instead of guessing it.
inline bool objcSynchronizedUnlockReturns(llvm::StringRef Source, size_t Exit,
                                          size_t NextExit) {
  const size_t End = Source.find(';', Exit);
  if (End == std::string::npos || End >= NextExit || NextExit > Source.size() ||
      NextExit - End >
          ::neverd::sdk::
              kObjCSynchronizedSourceObjcSynchronizedUnlockReturnsNextExitEndLimit)
    return false;
  enum class Lexical { Normal, String, Character, LineComment, BlockComment };
  Lexical State = Lexical::Normal;
  bool StatementStart = true, Returning = false;
  unsigned Parentheses = 0;
  for (size_t I = End + 1; I < NextExit; ++I) {
    const char C = Source[I], Next = I + 1 < NextExit ? Source[I + 1] : 0;
    if (State == Lexical::String || State == Lexical::Character) {
      if (C == '\\')
        ++I;
      else if (C == (State == Lexical::String ? '"' : '\''))
        State = Lexical::Normal;
      continue;
    }
    if (State == Lexical::LineComment) {
      if (C == '\n')
        State = Lexical::Normal;
      continue;
    }
    if (State == Lexical::BlockComment) {
      if (C == '*' && Next == '/') {
        State = Lexical::Normal;
        ++I;
      }
      continue;
    }
    if (C == '/' && (Next == '/' || Next == '*')) {
      State = Next == '/' ? Lexical::LineComment : Lexical::BlockComment;
      ++I;
      continue;
    }
    if (llvm::isSpace(C))
      continue;
    if (C == '"' || C == '\'') {
      State = C == '"' ? Lexical::String : Lexical::Character;
      StatementStart = false;
      continue;
    }
    if (C == '{' || C == '}' || C == '#' || (C == ':' && !Parentheses))
      return false;
    if (llvm::isAlpha(C) || C == '_') {
      const size_t Begin = I;
      while (I + 1 < NextExit &&
             (llvm::isAlnum(Source[I + 1]) || Source[I + 1] == '_'))
        ++I;
      const auto Token = Source.slice(Begin, I + 1);
      if (Token == "if" || Token == "switch" || Token == "for" ||
          Token == "while" || Token == "do" || Token == "goto" ||
          Token == "break" || Token == "continue")
        return false;
      if (Token == "return") {
        if (!StatementStart || Parentheses || Returning)
          return false;
        Returning = true;
      }
      StatementStart = false;
    } else if (C == '(') {
      ++Parentheses;
      StatementStart = false;
    } else if (C == ')') {
      if (!Parentheses)
        return false;
      --Parentheses;
    } else if (C == ';' && !Parentheses) {
      if (Returning)
        return true;
      StatementStart = true;
    } else
      StatementStart = false;
  }
  return false;
}

inline constexpr char ObjCSynchronizedCleanupDeclarations[] =
    "#include <stdint.h>\n"
    "#ifndef __EXCEPTIONS\n"
    "#error \"This source requires -fexceptions for Objective-C "
    "synchronization cleanup\"\n"
    "#endif\n"
    "extern int32_t neverd_darwin_objc_sync_exit(void*) "
    "__asm__(\"_objc_sync_exit\");\n"
    "static void neverd_objc_sync_cleanup(void **guard) {\n"
    "    if (*guard) (void)neverd_darwin_objc_sync_exit(*guard);\n"
    "}\n";

inline std::optional<std::string>
addObjCSynchronizedSequentialCleanup(llvm::StringRef Source,
                                     const ObjCSynchronizedSourceProof &Proof,
                                     llvm::StringRef FunctionName = {}) {
  const auto &Lifetimes = Proof.Lifetimes;
  const bool Flow = !Proof.NormalEnterCalls.empty();
  if ((!Flow &&
       (Lifetimes.size() < 2 ||
        Lifetimes.size() >
            ::neverd::sdk::
                kObjCSynchronizedSourceAddObjCSynchronizedSequentialCleanupLifetimesSizeLimit ||
        Proof.EnterCall != Lifetimes.front().EnterCall ||
        Proof.ExitCall != Lifetimes.back().ExitCall)) ||
      (Flow &&
       (!Lifetimes.empty() || !Proof.SplitResumeTail ||
        Proof.NormalEnterCalls.size() >
            ::neverd::sdk::
                kObjCSynchronizedSourceAddObjCSynchronizedSequentialCleanupProofNormalEnterCallsSizeLimit ||
        Proof.NormalExitCalls.empty() ||
        Proof.NormalExitCalls.size() >
            ::neverd::sdk::
                kObjCSynchronizedSourceAddObjCSynchronizedSequentialCleanupProofNormalExitCallsSizeLimit ||
        Proof.EnterCall != Proof.NormalEnterCalls.front() ||
        Proof.ExitCall != Proof.NormalExitCalls.back())) ||
      Proof.GuardStopCall != Proof.ExitCall || !Proof.ResumeTarget ||
      Proof.LandingPad <= Proof.ExitCall ||
      (Proof.LandingPadPrelude != 0 && Proof.LandingPadPrelude != 4) ||
      Proof.UnprotectedReleases || Proof.UnprotectedRetains ||
      Proof.ReceiverIsSavedLocal || Proof.ReceiverHasStackCopy ||
      (!Flow && Proof.SuspendARC))
    return std::nullopt;
  va_t PreviousExit = 0;
  for (const auto &Lifetime : Lifetimes) {
    if (Lifetime.EnterCall <= PreviousExit ||
        Lifetime.ExitCall <= Lifetime.EnterCall)
      return std::nullopt;
    PreviousExit = Lifetime.ExitCall;
  }
  for (const auto *Addresses :
       {&Proof.NormalEnterCalls, &Proof.NormalExitCalls}) {
    va_t Previous = 0;
    for (va_t Address : *Addresses) {
      if (Address <= Previous || Address >= Proof.LandingPad)
        return std::nullopt;
      Previous = Address;
    }
  }
  const size_t EnterCount =
      Flow ? Proof.NormalEnterCalls.size() : Lifetimes.size();
  const size_t ExitCount =
      Flow ? Proof.NormalExitCalls.size() : Lifetimes.size();
  const auto Body = objcSynchronizedSourceBody(Source, FunctionName);
  if (!Body || Source.slice(Body->first, Body->second)
                   .contains("neverd_objc_sync_guard"))
    return std::nullopt;
  // Collect actual runtime call tokens, excluding comments and literals.
  // Require every normal acquisition and exit certified by the machine CFG.
  // Source order need not alternate when mutually exclusive exits are cold.
  enum class Lexical { Normal, String, Character, LineComment, BlockComment };
  Lexical State = Lexical::Normal;
  std::vector<std::pair<size_t, bool>> Calls;
  for (size_t I = Body->first + 1; I < Body->second; ++I) {
    const char C = Source[I], Next = I + 1 < Body->second ? Source[I + 1] : 0;
    if (State == Lexical::String || State == Lexical::Character) {
      if (C == '\\')
        ++I;
      else if (C == (State == Lexical::String ? '"' : '\''))
        State = Lexical::Normal;
      continue;
    }
    if (State == Lexical::LineComment) {
      if (C == '\n')
        State = Lexical::Normal;
      continue;
    }
    if (State == Lexical::BlockComment) {
      if (C == '*' && Next == '/') {
        State = Lexical::Normal;
        ++I;
      }
      continue;
    }
    if (C == '/' && (Next == '/' || Next == '*')) {
      State = Next == '/' ? Lexical::LineComment : Lexical::BlockComment;
      ++I;
      continue;
    }
    if (C == '"' || C == '\'') {
      State = C == '"' ? Lexical::String : Lexical::Character;
      continue;
    }
    if (!llvm::isAlpha(C) && C != '_')
      continue;
    const size_t Begin = I;
    while (I + 1 < Body->second &&
           (llvm::isAlnum(Source[I + 1]) || Source[I + 1] == '_'))
      ++I;
    const auto Token = Source.slice(Begin, I + 1);
    const bool Enter = Token == "neverd_darwin_objc_sync_enter";
    if (!Enter && Token != "neverd_darwin_objc_sync_exit")
      continue;
    if (I + 1 >= Body->second || Source[I + 1] != '(' ||
        Calls.size() >= EnterCount + ExitCount)
      return std::nullopt;
    Calls.emplace_back(Begin, Enter);
  }
  if (Calls.size() != EnterCount + ExitCount ||
      static_cast<size_t>(
          std::count_if(Calls.begin(), Calls.end(), [](const auto &Call) {
            return Call.second;
          })) != EnterCount)
    return std::nullopt;
  std::vector<std::pair<size_t, std::string>> Edits;
  for (size_t I = 0; I < Calls.size(); ++I) {
    const auto [At, Enter] = Calls[I];
    if (!Flow && Enter != (I % 2 == 0))
      return std::nullopt;
    const llvm::StringRef Name = Enter ? "neverd_darwin_objc_sync_enter("
                                       : "neverd_darwin_objc_sync_exit(";
    const size_t Line = Source.rfind('\n', At);
    const size_t End = Source.find(';', At);
    if (Line == std::string::npos || End == std::string::npos ||
        End >= Body->second)
      return std::nullopt;
    const auto Statement = Source.slice(Line + 1, End + 1).trim();
    bool ExactSelfCall = false;
    for (llvm::StringRef Argument :
         {"objc_self)", "(void*)(uintptr_t)(objc_self))",
          "(void*)(uintptr_t)((uintptr_t)objc_self))"})
      ExactSelfCall |=
          Statement == ("(uint32_t)(" + Name + Argument + ");").str();
    if (!ExactSelfCall)
      return std::nullopt;
    size_t IndentEnd = Line + 1;
    while (IndentEnd < At &&
           (Source[IndentEnd] == ' ' || Source[IndentEnd] == '\t'))
      ++IndentEnd;
    const std::string Indent = Source.slice(Line + 1, IndentEnd).str();
    if (Enter)
      Edits.emplace_back(End + 1,
                         "\n" + Indent + "neverd_objc_sync_guard = objc_self;");
    else
      Edits.emplace_back(Line + 1, Indent + "neverd_objc_sync_guard = 0;\n");
  }
  std::string Result = Source.str();
  std::sort(Edits.begin(), Edits.end(),
            [](const auto &A, const auto &B) { return A.first > B.first; });
  for (const auto &[At, Text] : Edits)
    Result.insert(At, Text);
  Result.insert(Body->first + 1,
                "\n    void *neverd_objc_sync_guard "
                "__attribute__((cleanup(neverd_objc_sync_cleanup))) = 0;");
  if (!Source.contains(ObjCSynchronizedCleanupDeclarations))
    Result.insert(0, ObjCSynchronizedCleanupDeclarations);
  if (Flow && Proof.SuspendARC) {
    const auto RenderedBody = objcSynchronizedSourceBody(Result, FunctionName);
    if (!RenderedBody)
      return std::nullopt;
    return objcSynchronizedSuspendRuntimeCalls(Result, RenderedBody->first + 1,
                                               RenderedBody->second,
                                               Proof.SuspendARC, "objc_self");
  }
  return Result;
}

inline std::optional<std::string>
addObjCSynchronizedReceiverCleanup(llvm::StringRef Source,
                                   const ObjCSynchronizedSourceProof &Proof,
                                   llvm::StringRef FunctionName = {}) {
  if (Proof.SuspendARC > 3 ||
      (Proof.SuspendDispatchLeave && !Proof.PrivateFrame))
    return std::nullopt;
  if (!Proof.Lifetimes.empty() || !Proof.NormalEnterCalls.empty())
    return addObjCSynchronizedSequentialCleanup(Source, Proof, FunctionName);
  const std::string EnterName = "neverd_darwin_objc_sync_enter(";
  const std::string DispatchName = "neverd_darwin_dispatch_group_enter(";
  const std::string ExitName = "neverd_darwin_objc_sync_exit(";
  const std::string ReleaseName = "objc_release(";
  const std::string RetainName = "objc_retain(";
  const auto Body = objcSynchronizedSourceBody(Source, FunctionName);
  if (!Body)
    return std::nullopt;
  if (Source.slice(Body->first, Body->second)
          .contains("neverd_objc_sync_guard"))
    return std::nullopt;
  const auto Scoped = Source.take_front(Body->second);
  const size_t Open = Body->first;
  const size_t Enter = Scoped.find(EnterName, Open);
  const size_t Dispatch = Scoped.find(DispatchName, Open);
  const size_t Exit = Scoped.find(ExitName, Open);
  std::vector<size_t> Exits;
  for (size_t At = Exit; At != std::string::npos;
       At = Scoped.find(ExitName, At + 1)) {
    if (Exits.size() ==
        ::neverd::sdk::
            kObjCSynchronizedSourceAddObjCSynchronizedReceiverCleanupExitsSizeLimit)
      return std::nullopt;
    Exits.push_back(At);
  }
  const bool StopAtExit = Proof.GuardStopCall == Proof.ExitCall;
  const size_t Release = Proof.UnprotectedReleases
                             ? Scoped.find(ReleaseName, Enter)
                             : std::string::npos;
  const size_t Retain = Proof.UnprotectedRetains
                            ? Scoped.find(RetainName, Enter)
                            : std::string::npos;
  const size_t Stop = StopAtExit                  ? Exit
                      : Proof.UnprotectedRetains  ? Retain
                      : Proof.UnprotectedReleases ? Release
                                                  : Dispatch;
  if (Enter == std::string::npos || Stop == std::string::npos ||
      Exit == std::string::npos ||
      Scoped.find(EnterName, Enter + 1) != std::string::npos ||
      (Exits.size() > 1 && (!StopAtExit || Proof.ReceiverIsSavedLocal)) ||
      (Proof.UnprotectedReleases && (Release <= Enter || Release >= Exit)) ||
      (Proof.UnprotectedRetains &&
       (Proof.UnprotectedRetains != 1 || Retain <= Enter || Retain >= Exit ||
        (Proof.UnprotectedReleases && Retain >= Release) ||
        Scoped.find(RetainName, Retain + 1) < Exit)) ||
      (!StopAtExit && !Proof.UnprotectedReleases && !Proof.UnprotectedRetains &&
       Scoped.find(DispatchName, Dispatch + 1) != std::string::npos) ||
      Enter >= Stop || Stop > Exit)
    return std::nullopt;
  if (Proof.UnprotectedReleases) {
    size_t Count = 0;
    for (size_t At = Release; At != std::string::npos && At < Exit;
         At = Scoped.find(ReleaseName, At + 1))
      ++Count;
    if (Count != Proof.UnprotectedReleases)
      return std::nullopt;
  }
  const size_t EnterEnd = Source.find(';', Enter);
  if (EnterEnd == std::string::npos)
    return std::nullopt;
  if (Proof.SuspendARC || Proof.SuspendDispatchLeave) {
    if (!StopAtExit || Proof.UnprotectedReleases || Proof.UnprotectedRetains ||
        (!Proof.ReceiverIsSavedLocal &&
         (!objcSynchronizedSelfArgument(Scoped, Enter, EnterName) ||
          !objcSynchronizedSelfArgument(Scoped, Exit, ExitName))))
      return std::nullopt;
  }
  std::string Receiver = "objc_self";
  if (Proof.ReceiverIsSavedLocal) {
    const auto EnterArg =
        objcSynchronizedSavedLocalArgument(Source, Enter, EnterName);
    const auto ExitArg =
        objcSynchronizedSavedLocalArgument(Source, Exit, ExitName);
    if (!EnterArg || !ExitArg)
      return std::nullopt;
    const auto LocalName = [](llvm::StringRef Argument) {
      return Argument.drop_front(llvm::StringRef("(void*)(uintptr_t)(").size())
          .drop_back();
    };
    const auto EnterLocal = LocalName(*EnterArg);
    const auto ExitLocal = LocalName(*ExitArg);
    if (*EnterArg != *ExitArg) {
      if (!Proof.ReceiverHasStackCopy)
        return std::nullopt;
      const std::string Copy = (ExitLocal + " = " + EnterLocal + ";").str();
      const size_t At = Scoped.find(Copy, Open);
      const size_t Line =
          At == std::string::npos ? std::string::npos : Scoped.rfind('\n', At);
      const size_t LineEnd =
          At == std::string::npos ? std::string::npos : Scoped.find('\n', At);
      if (At == std::string::npos || Line == std::string::npos ||
          LineEnd == std::string::npos || LineEnd >= Enter ||
          Scoped.slice(Line + 1, LineEnd).trim() != Copy ||
          Scoped.slice(At + Copy.size(), Enter).contains(EnterLocal) ||
          Scoped.slice(At + Copy.size(), Enter).contains(ExitLocal))
        return std::nullopt;
    }
    if (Source.slice(EnterEnd + 1, Exit).contains(EnterLocal) ||
        Source.slice(EnterEnd + 1, Exit).contains(ExitLocal))
      return std::nullopt;
    Receiver = *EnterArg;
  }
  const size_t StopLine = Source.rfind('\n', Stop);
  const size_t ExitLine = Source.rfind('\n', Exit);
  if (EnterEnd == std::string::npos || StopLine == std::string::npos ||
      ExitLine == std::string::npos || EnterEnd >= StopLine ||
      (!StopAtExit && StopLine >= ExitLine) ||
      (!StopAtExit &&
       Source.substr(StopLine + 1, Stop - StopLine - 1).trim() != "" &&
       (!Proof.UnprotectedRetains ||
        (Source.substr(StopLine + 1, Stop - StopLine - 1).trim() !=
             "(uint64_t)(" &&
         Source.substr(StopLine + 1, Stop - StopLine - 1).trim() !=
             "(uint64_t)(uintptr_t)("))) ||
      Source.substr(ExitLine + 1, Exit - ExitLine - 1).trim() != "(uint32_t)(")
    return std::nullopt;
  std::string Result = Source.str();
  if (Exits.size() > 1) {
    // HighC can expand a shared unlock tail into mutually exclusive returns.
    // Each occurrence must still use the proved self receiver and be a whole
    // statement. Clear the guard on every rendered normal-unlock path.
    if (!objcSynchronizedSelfArgument(Scoped, Enter, EnterName))
      return std::nullopt;
    for (size_t I = 1; I < Exits.size(); ++I)
      if (!objcSynchronizedUnlockReturns(Scoped, Exits[I - 1], Exits[I]))
        return std::nullopt;
    for (auto It = Exits.rbegin(); It != Exits.rend(); ++It) {
      const size_t Line = Source.rfind('\n', *It);
      if (*It <= EnterEnd || Line == std::string::npos ||
          Scoped.slice(Line + 1, *It).trim() != "(uint32_t)(" ||
          !objcSynchronizedSelfArgument(Scoped, *It, ExitName))
        return std::nullopt;
      size_t IndentEnd = Line + 1;
      while (IndentEnd < *It &&
             (Source[IndentEnd] == ' ' || Source[IndentEnd] == '\t'))
        ++IndentEnd;
      Result.insert(Line + 1, Source.slice(Line + 1, IndentEnd).str() +
                                  "neverd_objc_sync_guard = 0;\n");
    }
  } else
    Result.insert(StopLine + 1, "    neverd_objc_sync_guard = 0;\n");
  Result.insert(EnterEnd + 1,
                "\n    neverd_objc_sync_guard = " + Receiver + ";");
  Result.insert(Open + 1,
                "\n    void *neverd_objc_sync_guard "
                "__attribute__((cleanup(neverd_objc_sync_cleanup))) = 0;");
  if (!Source.contains(ObjCSynchronizedCleanupDeclarations))
    Result.insert(0, ObjCSynchronizedCleanupDeclarations);
  if (Proof.SuspendARC || Proof.SuspendDispatchLeave) {
    const auto RenderedBody = objcSynchronizedSourceBody(Result, FunctionName);
    if (!RenderedBody)
      return std::nullopt;
    const size_t RenderedEnter = Result.find(EnterName, RenderedBody->first);
    // A shared machine unlock can be duplicated into early HighC returns.
    // ARC calls after an earlier rendered unlock must leave its guard zero;
    // suspend every occurrence through the last normal unlock, preserving
    // whether that path still owns the lock.
    const size_t RenderedExit = Result.rfind(ExitName, RenderedBody->second);
    const size_t RenderedEnterEnd = Result.find(';', RenderedEnter);
    return objcSynchronizedSuspendRuntimeCalls(
        Result, RenderedEnterEnd + 1, RenderedExit,
        Proof.SuspendARC | (Proof.SuspendDispatchLeave ? 4 : 0), Receiver);
  }
  return Result;
}

} // namespace neverd::sdk

#endif
