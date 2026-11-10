//===- X86Paging.cpp - Architectural x86 page walks -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Intel SDM volume 3A, chapter 5: legacy, PAE, four- and five-level paging.
//===----------------------------------------------------------------------===//

#include "neverd/translate/X86/X86Paging.h"

#include "llvm/Support/Errc.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <utility>

namespace neverd::translate {
namespace {

constexpr uint64_t bit(unsigned N) { return uint64_t(1) << N; }
constexpr uint64_t lowBits(unsigned N) { return bit(N) - 1; }
constexpr uint64_t Present = bit(0), Writable = bit(1), User = bit(2),
                   Accessed = bit(5), Dirty = bit(6), Large = bit(7),
                   NX = bit(63);
constexpr uint64_t PG = bit(31), WP = bit(16), PE = bit(0);
constexpr uint64_t PSE = bit(4), PAE = bit(5), LA57 = bit(12), SMEP = bit(20),
                   SMAP = bit(21), PKE = bit(22), CET = bit(23), PKS = bit(24);
constexpr uint64_t LME = bit(8), LMA = bit(10), NXE = bit(11), AC = bit(18);

llvm::Error invalid(const char *Message) {
  return llvm::createStringError(llvm::errc::invalid_argument, Message);
}

bool validFeatures(const X86PagingFeatures &F) {
  return F.PhysicalAddressBits >= 32 && F.PhysicalAddressBits <= 52;
}

uint64_t paeReserved(const X86PagingFeatures &F) {
  return ~lowBits(F.PhysicalAddressBits) | 0x1e6;
}

class Walker {
public:
  Walker(GuestMemoryRuntime &Memory, const X86PagingFeatures &Features,
         const X86PagingState &State, const X86LinearAccess &Access)
      : Memory(Memory), F(Features), S(State), A(Access),
        LongMode(S.EFER & LMA), Legacy(!(S.CR4 & PAE)),
        UserAccess(A.CPL == 3 && !A.ImplicitSupervisor),
        Write(A.Kind == MemoryAccessKind::Write),
        Execute(A.Kind == MemoryAccessKind::Execute) {}

  X86PageWalkResult run() {
    if (!(S.CR0 & PG)) {
      Result.PhysicalAddress = A.Address;
      Result.PageSize = bit(32);
      return std::move(Result);
    }

    uint64_t Table = S.CR3 & lowBits(F.PhysicalAddressBits) & ~lowBits(12);
    unsigned Level = LongMode ? (S.CR4 & LA57 ? 5 : 4) : 2;
    if (!LongMode) {
      if (Legacy)
        Table = S.CR3 & 0xfffff000;
      else {
        const uint64_t Entry = S.PDPTE[(A.Address >> 30) & 3];
        if (!(Entry & Present)) {
          pageFault(0);
          return std::move(Result);
        }
        Table = Entry & lowBits(F.PhysicalAddressBits) & ~lowBits(12);
      }
    }

    bool AllowUser = true, AllowWrite = true, AllowExecute = true;
    for (; Level; --Level) {
      const unsigned Shift =
          Legacy ? (Level == 2 ? 22 : 12) : 12 + 9 * (Level - 1);
      const uint64_t Index = (A.Address >> Shift) & (Legacy ? 1023 : 511);
      const uint64_t EntryAddress = Table + Index * (Legacy ? 4 : 8);
      const auto Width = Legacy ? GuestScalarWidth::I32 : GuestScalarWidth::I64;
      auto Read = Memory.loadScalar(EntryAddress, Width);
      if (backing(Read))
        break;
      uint64_t Entry = Read.Value;
      if (!(Entry & Present)) {
        pageFault(0);
        break;
      }
      const bool IsLarge =
          Level > 1 && (Entry & Large) && (!Legacy || (S.CR4 & PSE));
      const bool Leaf = Level == 1 || IsLarge;
      if (reserved(Entry, Level, IsLarge)) {
        pageFault(1 | 8);
        break;
      }

      AllowUser &= bool(Entry & User);
      AllowWrite &= bool(Entry & Writable);
      AllowExecute &= Legacy || !(Entry & NX);
      // A fresh walk may set A on valid entries before finding a later fault.
      // Do not touch a non-present or reserved entry, or the cached PAE bank.
      if (!update(EntryAddress, Width, Entry, Accessed))
        break;
      if (!Leaf) {
        Table = Entry & lowBits(F.PhysicalAddressBits) & ~lowBits(12);
        continue;
      }

      bool KeyDenied = false;
      if (LongMode && !Execute) {
        const unsigned Key = (Entry >> 59) & 15;
        const uint32_t Rights = AllowUser ? S.PKRU : S.PKRS;
        if (S.CR4 & (AllowUser ? PKE : PKS)) {
          const unsigned Pair = (Rights >> (2 * Key)) & 3;
          KeyDenied = (Pair & 1) ||
                      ((Pair & 2) && Write && (UserAccess || (S.CR0 & WP)));
        }
      }
      const bool Denied =
          (UserAccess && !AllowUser) ||
          (Write && !AllowWrite && (UserAccess || (S.CR0 & WP))) ||
          (Execute &&
           (!AllowExecute || (!UserAccess && AllowUser && (S.CR4 & SMEP)))) ||
          (!Execute && !UserAccess && AllowUser && (S.CR4 & SMAP) &&
           (A.ImplicitSupervisor || !(S.RFlags & AC))) ||
          KeyDenied;
      if (Denied) {
        pageFault(1 | (KeyDenied ? 32 : 0));
        break;
      }
      // An admitted supervisor write with WP=0 still sets D in a read-only
      // leaf. CET's concurrent R/W-clearing guarantee does not change this
      // case; CR4.CET itself requires WP=1 and is a separate access contract.
      if (Write && !update(EntryAddress, Width, Entry, Dirty))
        break;
      const unsigned PageBits = Leaf && IsLarge ? Shift : 12;
      uint64_t Base =
          Entry & lowBits(F.PhysicalAddressBits) & ~lowBits(PageBits);
      if (Legacy && IsLarge) {
        Base = Entry & 0xffc00000;
        if (F.PSE36)
          Base |= (Entry & 0x001fe000) << 19;
      }
      Result.PageSize = bit(PageBits);
      Result.PhysicalAddress = Base | (A.Address & lowBits(PageBits));
      break;
    }
    return std::move(Result);
  }

private:
  bool backing(const GuestMemoryAccessResult &R) {
    if (R.Status == GuestMemoryAccessStatus::Fault) {
      Result.Status = X86PageWalkStatus::BackingFault;
      Result.BackingFault = R.Fault;
      return true;
    }
    if (R.SelfModification)
      Result.SelfModifications.push_back(*R.SelfModification);
    return false;
  }

  bool update(uint64_t Address, GuestScalarWidth Width, uint64_t &Entry,
              uint64_t Flags) {
    if ((Entry & Flags) == Flags)
      return true;
    if (backing(Memory.storeScalar(Address, Width, Entry | Flags)))
      return false;
    Entry |= Flags;
    return true;
  }

  void pageFault(uint32_t Reason) {
    const bool FetchBit =
        Execute && ((S.CR4 & SMEP) || ((S.CR4 & PAE) && (S.EFER & NXE)));
    Result.Status = X86PageWalkStatus::Exception;
    Result.Exception =
        X86MemoryException{14,
                           Reason | (Write ? 2u : 0u) | (UserAccess ? 4u : 0u) |
                               (FetchBit ? 16u : 0u),
                           A.Address};
  }

  bool reserved(uint64_t E, unsigned Level, bool IsLarge) const {
    if (Legacy) {
      if (IsLarge) {
        const unsigned M =
            F.PSE36 ? std::min<unsigned>(40, F.PhysicalAddressBits) : 32;
        if (E & (lowBits(22) & ~lowBits(M - 19)))
          return true;
      }
    } else {
      const uint64_t HighReserved = (LongMode ? lowBits(52) : lowBits(63)) &
                                    ~lowBits(F.PhysicalAddressBits);
      if ((E & HighReserved) || (!(S.EFER & NXE) && (E & NX)))
        return true;
      if (IsLarge && (Level > 3 || (Level == 3 && !F.Page1GB)))
        return true;
      if (IsLarge && (E & (lowBits(12 + 9 * (Level - 1)) & ~lowBits(13))))
        return true;
    }
    if (!F.PAT && ((Level == 1 && (E & Large)) || (IsLarge && (E & bit(12)))))
      return true;
    return false;
  }

  GuestMemoryRuntime &Memory;
  const X86PagingFeatures &F;
  const X86PagingState &S;
  const X86LinearAccess &A;
  bool LongMode, Legacy, UserAccess, Write, Execute;
  X86PageWalkResult Result;
};

} // namespace

llvm::Error validateX86PagingState(const X86PagingFeatures &F,
                                   const X86PagingState &S) {
  if (!validFeatures(F))
    return invalid("x86 paging requires MAXPHYADDR in 32..52");
  if ((S.CR0 & PG) && !(S.CR0 & PE))
    return invalid("x86 paging requires protected mode");
  if (((S.CR4 & PSE) && !F.PSE) || ((S.CR4 & PAE) && !F.PAE) ||
      ((S.EFER & LME) && !F.LongMode))
    return invalid("paging mode is not supported by the bound CPU");
  if (bool(S.EFER & LMA) != bool((S.CR0 & PG) && (S.EFER & LME)))
    return invalid("x86 paging has inconsistent LMA/LME/PG state");
  if ((S.EFER & LMA) && !(S.CR4 & PAE))
    return invalid("IA-32e paging requires PAE");
  if ((S.CR4 & LA57) && !F.LA57)
    return invalid("LA57 is not supported by the bound CPU");
  if ((S.EFER & NXE) && !F.NX)
    return invalid("NXE is not supported by the bound CPU");
  if (((S.CR4 & SMEP) && !F.SMEP) || ((S.CR4 & SMAP) && !F.SMAP) ||
      ((S.CR4 & PKE) && !F.PKU) || ((S.CR4 & PKS) && !F.PKS))
    return invalid(
        "paging protection control is not supported by the bound CPU");
  if ((S.CR4 & (CET | bit(27) | bit(28))) ||
      ((S.EFER & LMA) && (S.CR3 & (bit(61) | bit(62)))))
    return invalid(
        "CET, LASS and LAM require a separate memory-access contract");
  if ((S.EFER & LMA) && (S.CR3 & ~lowBits(F.PhysicalAddressBits)))
    return invalid("IA-32e CR3 has reserved physical-address bits");
  if ((S.CR0 & PG) && (S.CR4 & PAE) && !(S.EFER & LMA))
    for (uint64_t Entry : S.PDPTE)
      if ((Entry & Present) && (Entry & paeReserved(F)))
        return invalid("PAE register bank contains a reserved present PDPTE");
  return llvm::Error::success();
}

llvm::Expected<std::optional<X86MemoryException>>
checkX86LinearAccess(const X86PagingState &S, const X86LinearAccess &A,
                     uint64_t Bytes) {
  if (A.CPL > 3 ||
      (A.Kind != MemoryAccessKind::Read && A.Kind != MemoryAccessKind::Write &&
       A.Kind != MemoryAccessKind::Execute))
    return invalid("invalid x86 paging access kind or privilege");
  if (A.Kind == MemoryAccessKind::Execute && (A.ImplicitSupervisor || A.Stack))
    return invalid("instruction fetch cannot be an implicit or stack access");
  if (A.Wrap32 && (S.EFER & LMA) && A.ImplicitSupervisor)
    return invalid("IA-32e system-table operands require full-width addresses");
  if (!Bytes || Bytes > kMemoryFaultMaxAccessBytes)
    return invalid(
        "x86 paged operand requires a nonempty representable extent");
  if (!(S.EFER & LMA) || A.Wrap32) {
    if (A.Address > UINT32_MAX ||
        (!A.Wrap32 && Bytes - 1 > UINT32_MAX - A.Address))
      return invalid(
          "32-bit x86 wrap requires a checked segment or legacy table operand");
  } else {
    const unsigned Width = S.CR4 & LA57 ? 57 : 48;
    for (uint64_t Address : {A.Address, A.Address + Bytes - 1}) {
      const uint64_t High = Address >> (Width - 1);
      if (High && High != lowBits(65 - Width))
        return std::optional<X86MemoryException>(
            X86MemoryException{uint8_t(A.Stack ? 12 : 13), 0, std::nullopt});
    }
  }
  return std::optional<X86MemoryException>();
}

llvm::Expected<X86PageWalkResult> walkX86PageTables(GuestMemoryRuntime &Memory,
                                                    const X86PagingFeatures &F,
                                                    const X86PagingState &S,
                                                    const X86LinearAccess &A) {
  if (auto Error = validateX86PagingState(F, S))
    return std::move(Error);
  auto Check = checkX86LinearAccess(S, A, 1);
  if (!Check)
    return Check.takeError();
  if (*Check) {
    X86PageWalkResult Result;
    Result.Status = X86PageWalkStatus::Exception;
    Result.Exception = *Check;
    return Result;
  }
  return Walker(Memory, F, S, A).run();
}

llvm::Expected<X86PageWalkResult>
loadX86PAEPDPTE(GuestMemoryRuntime &Memory, const X86PagingFeatures &F,
                uint64_t CR3, std::array<uint64_t, 4> &PDPTE) {
  if (!validFeatures(F))
    return invalid("x86 paging requires MAXPHYADDR in 32..52");
  if (!F.PAE)
    return invalid("PDPTE reload requires a CPU with PAE");
  X86PageWalkResult Result;
  std::array<uint64_t, 4> Loaded{};
  for (unsigned I = 0; I != Loaded.size(); ++I) {
    auto Read =
        Memory.loadScalar((CR3 & 0xffffffe0) + I * 8, GuestScalarWidth::I64);
    if (Read.Status == GuestMemoryAccessStatus::Fault) {
      Result.Status = X86PageWalkStatus::BackingFault;
      Result.BackingFault = Read.Fault;
      return Result;
    }
    Loaded[I] = Read.Value;
    if ((Loaded[I] & Present) && (Loaded[I] & paeReserved(F))) {
      Result.Status = X86PageWalkStatus::Exception;
      Result.Exception = X86MemoryException{13, 0, std::nullopt};
      return Result;
    }
  }
  PDPTE = Loaded;
  return Result;
}

} // namespace neverd::translate
