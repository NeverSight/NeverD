//===- X86PagingNativeTests.cpp - Independent CPU paging oracle -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Execute original MOV/fetch instructions in KVM and compare architectural
/// success, #PF error codes and CR2 with the software page walker.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/analysis/arch/x86_64/X64PagedEventDelivery.h"
#include "neverd/translate/X86/X86LegacyExecution.h"
#include "neverd/translate/X86/X86LegacySegmentExecution.h"
#include "neverd/translate/X86/X86PagedMemory.h"
#include "neverd/translate/X86/X86Paging.h"

#include "llvm/Support/Error.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

#if defined(__linux__) && defined(__x86_64__)
#include <cerrno>
#include <fcntl.h>
#include <linux/kvm.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

using namespace neverd::translate;
using namespace neverd::analysis;

namespace {

#if defined(__linux__) && defined(__x86_64__)

class X86PagingNativeTest : public testing::TestWithParam<unsigned> {
protected:
  static constexpr uint64_t MemorySize = 0x200000;
  static constexpr uint64_t DataPage = 0x180000;
  static constexpr uint64_t SecondDataPage = 0x1a0000;
  static constexpr uint64_t WrappedPage = 0x1b0000;
  static constexpr uint64_t Code = 0x1000;
  static constexpr uint64_t Handler = 0x2000;
  static constexpr uint64_t Stack = 0x1e0000;
  static constexpr uint64_t UserStack = 0x1d0000;
  int KVM = -1, VM = -1, CPU = -1;
  int RunSize = 0;
  uint8_t *RAM = nullptr;
  kvm_run *Run = nullptr;
  std::vector<uint64_t> CPUIDStorage;
  X86PagingFeatures Features;
  kvm_sregs Initial{};
  unsigned EventComparisons = 0;

  void SetUp() override {
    KVM = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (KVM < 0)
      GTEST_SKIP() << "KVM unavailable: " << errno;
    ASSERT_EQ(ioctl(KVM, KVM_GET_API_VERSION, 0), KVM_API_VERSION);
    VM = ioctl(KVM, KVM_CREATE_VM, 0);
    ASSERT_GE(VM, 0);
    RAM =
        static_cast<uint8_t *>(mmap(nullptr, MemorySize, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    ASSERT_NE(RAM, MAP_FAILED);
    kvm_userspace_memory_region Region{};
    Region.memory_size = MemorySize;
    Region.userspace_addr = reinterpret_cast<uintptr_t>(RAM);
    ASSERT_EQ(ioctl(VM, KVM_SET_USER_MEMORY_REGION, &Region), 0);
    CPU = ioctl(VM, KVM_CREATE_VCPU, 0);
    ASSERT_GE(CPU, 0);
    constexpr unsigned Capacity = 256;
    CPUIDStorage.resize((sizeof(kvm_cpuid2) +
                         Capacity * sizeof(kvm_cpuid_entry2) +
                         sizeof(uint64_t) - 1) /
                        sizeof(uint64_t));
    auto *CPUID = reinterpret_cast<kvm_cpuid2 *>(CPUIDStorage.data());
    CPUID->nent = Capacity;
    ASSERT_EQ(ioctl(KVM, KVM_GET_SUPPORTED_CPUID, CPUID), 0);
    ASSERT_EQ(ioctl(CPU, KVM_SET_CPUID2, CPUID), 0);
    for (unsigned I = 0; I != CPUID->nent; ++I) {
      const auto &E = CPUID->entries[I];
      if (E.function == 1) {
        Features.PSE = E.edx & (1u << 3);
        Features.PAE = E.edx & (1u << 6);
        Features.PAT = E.edx & (1u << 16);
        Features.PSE36 = E.edx & (1u << 17);
      }
      if (E.function == 7 && E.index == 0) {
        Features.SMEP = E.ebx & (1u << 7);
        Features.SMAP = E.ebx & (1u << 20);
        Features.LA57 = E.ecx & (1u << 16);
        Features.PKU = E.ecx & (1u << 3);
        Features.PKS = E.ecx & (1u << 31);
      }
      if (E.function == 0x80000001) {
        Features.LongMode = E.edx & (1u << 29);
        Features.NX = E.edx & (1u << 20);
        Features.Page1GB = E.edx & (1u << 26);
      }
      if (E.function == 0x80000008)
        Features.PhysicalAddressBits = E.eax & 0xff;
    }
    if (GetParam() == 5 && !Features.LA57)
      GTEST_SKIP() << "Bound KVM CPU has no LA57";
    ASSERT_TRUE(Features.NX);
    RunSize = ioctl(KVM, KVM_GET_VCPU_MMAP_SIZE, 0);
    ASSERT_GT(RunSize, 0);
    Run = static_cast<kvm_run *>(
        mmap(nullptr, RunSize, PROT_READ | PROT_WRITE, MAP_SHARED, CPU, 0));
    ASSERT_NE(Run, MAP_FAILED);
    ASSERT_EQ(ioctl(CPU, KVM_GET_SREGS, &Initial), 0);
  }

  void TearDown() override {
    if (Run && Run != MAP_FAILED)
      munmap(Run, RunSize);
    if (RAM && RAM != MAP_FAILED)
      munmap(RAM, MemorySize);
    for (int FD : {CPU, VM, KVM})
      if (FD >= 0)
        close(FD);
  }

  static uint64_t physicalByte(uint64_t Address) {
    return Address >= UINT64_MAX - 4095 ? WrappedPage + (Address & 4095)
                                        : Address;
  }

  void put(uint64_t Address, uint64_t Value, unsigned Bytes = 8) {
    for (unsigned I = 0; I != Bytes; ++I) {
      const auto Physical = physicalByte(Address + I);
      ASSERT_LT(Physical, MemorySize);
      RAM[Physical] = Value >> (I * 8);
    }
  }

  uint64_t get(uint64_t Address, unsigned Bytes = 8) {
    uint64_t Value = 0;
    for (unsigned I = 0; I != Bytes; ++I) {
      const auto Physical = physicalByte(Address + I);
      EXPECT_LT(Physical, MemorySize);
      if (Physical >= MemorySize)
        return 0;
      Value |= uint64_t(RAM[Physical]) << (I * 8);
    }
    return Value;
  }

  static kvm_segment segment(unsigned Selector, bool CodeSegment, bool Wide,
                             unsigned CPL) {
    kvm_segment S{};
    S.limit = UINT32_MAX;
    S.selector = Selector;
    S.type = CodeSegment ? 11 : 3;
    S.present = 1;
    S.s = 1;
    S.g = 1;
    S.dpl = CPL;
    S.l = CodeSegment && Wide;
    S.db = !S.l;
    return S;
  }

  void compare(unsigned ChangeLevel, uint64_t Clear, uint64_t Set,
               MemoryAccessKind Kind, unsigned CPL, bool WriteProtect,
               unsigned Protection, bool AC, unsigned FirstPageBytes = 0,
               bool SecondPagePresent = true, unsigned DeliveryFailure = 0,
               unsigned AddressBoundary = 0, bool StackReference = false,
               unsigned InitialEvent = 0, unsigned WrappingTable = 0) {
    const bool Wide = GetParam() >= 4;
    const bool Legacy = GetParam() == 2;
    const unsigned Width = Legacy ? 4 : 8;
    const unsigned Levels = Wide ? GetParam() : 2;
    const uint64_t Page =
        AddressBoundary == 2 ? UINT64_MAX - 4095
        : AddressBoundary == 1
            ? (uint64_t(1) << (GetParam() == 5 ? 56 : 47)) - 4096
        : Wide ? (GetParam() == 5 ? uint64_t(1) << 48 : uint64_t(1) << 39)
               : 0x40000000;
    const uint64_t Offset = FirstPageBytes ? 4096 - FirstPageBytes : 0;
    const uint64_t Address = Page + Offset;
    const uint64_t IDTBase = WrappingTable == 1     ? UINT64_MAX - 127
                             : WrappingTable == 6   ? UINT64_MAX - 14 * 16 - 7
                             : DeliveryFailure >= 5 ? 0x6f30
                                                    : 0x6000;
    const uint64_t GDTBase = WrappingTable == 2   ? UINT64_MAX - 7
                             : WrappingTable == 7 ? UINT64_MAX - 11
                                                  : 0x5000;
    const uint64_t TSSBase = WrappingTable == 3   ? UINT64_MAX - 3
                             : WrappingTable == 8 ? UINT64_MAX - 7
                                                  : 0x5800;
    const uint64_t KernelSP = WrappingTable == 4     ? 0
                              : WrappingTable == 5   ? 16
                              : DeliveryFailure == 7 ? Stack + 16
                                                     : Stack;
    SCOPED_TRACE(testing::Message()
                 << "mode=" << GetParam() << " level=" << ChangeLevel
                 << " clear=" << Clear << " set=" << Set
                 << " kind=" << unsigned(Kind) << " cpl=" << CPL
                 << " wp=" << WriteProtect << " protection=" << Protection
                 << " ac=" << AC << " prefix=" << FirstPageBytes << " second="
                 << SecondPagePresent << " delivery=" << DeliveryFailure
                 << " address_boundary=" << AddressBoundary
                 << " stack=" << StackReference << " event=" << InitialEvent
                 << " wrapped_table=" << WrappingTable);
    std::memset(RAM, 0, MemorySize);
    // Separate kernel and tested hierarchies let faults at every level retain
    // an independent, mapped exception entry and supervisor stack.
    if (Wide) {
      put(0x10000, 0x11007);
      put(0x11000, 0x12007);
      put(0x12000, 0x13007);
      if (GetParam() == 5)
        put(0x13000, 0x14007);
    } else if (Legacy) {
      put(0x10000, 0x11007, 4);
    } else {
      put(0x10000, 0x11001);
      put(0x11000, 0x12007);
      put(0x10008, 0x20001);
    }
    const uint64_t KernelPT = Wide ? (GetParam() == 5 ? 0x14000 : 0x13000)
                                   : (Legacy ? 0x11000 : 0x12000);
    for (unsigned I = 0; I != MemorySize / 4096; ++I) {
      const uint64_t Page = I * 4096;
      const bool User = (Page == Code && CPL == 3) || Page == UserStack - 4096;
      put(KernelPT + I * Width, Page | (User ? 7 : 3), Width);
    }
    if (AddressBoundary == 2)
      put(KernelPT, SecondDataPage | 7, Width);
    if (WrappingTable >= 4)
      for (unsigned I = 0; I != Levels; ++I) {
        const uint64_t Table = I ? 0x60000 + (I - 1) * 0x1000 : 0x10000;
        put(Table + 511 * 8,
            (I + 1 == Levels ? WrappedPage : 0x60000 + I * 0x1000) | 3);
      }
    std::vector<uint64_t> Entries;
    if (Wide) {
      for (unsigned I = 0; I != Levels; ++I) {
        const uint64_t Table = I ? 0x20000 + (I - 1) * 0x1000 : 0x10000;
        Entries.push_back(Table +
                          ((Page >> (12 + 9 * (Levels - I - 1))) & 511) * 8);
      }
    } else {
      Entries = {Legacy ? 0x10400u : 0x20000u, 0x21000};
    }
    for (unsigned I = 0; I != Entries.size(); ++I)
      put(Entries[I],
          (I + 1 == Entries.size() ? DataPage
                                   : Entries[I + 1] & ~uint64_t(4095)) |
              7,
          Width);
    put(Entries.back() + Width, SecondPagePresent ? SecondDataPage | 7 : 0,
        Width);
    if (ChangeLevel < Entries.size()) {
      const uint64_t Entry = Entries[ChangeLevel];
      put(Entry, (get(Entry, Width) & ~Clear) | Set, Width);
    }
    // A split descriptor must already have A=1: the host enables split-lock
    // detection, and its implicit locked A update raises #AC during #PF entry.
    // KVM reports that pair as KVM_INTERNAL_ERROR_SIMUL_EX. This case qualifies
    // the wrapped descriptor read; it does not qualify bus-lock fault routing.
    const uint64_t CodeDescriptor =
        (Wide ? 0x00af9a000000ffffULL : 0x00cf9a000000ffffULL) |
        (WrappingTable == 7 ? uint64_t(1) << 40 : 0);
    put(GDTBase + 8, CodeDescriptor);
    put(GDTBase + 16, 0x00cf93000000ffffULL);
    put(GDTBase + 24, CodeDescriptor | 0x600000000000ULL);
    put(GDTBase + 32, 0x00cff3000000ffffULL);
    put(TSSBase + 4, KernelSP, Wide ? 8 : 4);
    if (!Wide)
      put(TSSBase + 8, 16, 2);
    for (unsigned Vector : {2u, 6u, 8u, 12u, 13u, 14u}) {
      const uint64_t Target = Handler + Vector * 16;
      const uint64_t Gate = IDTBase + Vector * (Wide ? 16 : 8);
      if (Gate < MemorySize ||
          (WrappingTable >= 4 && Gate >= UINT64_MAX - 4095))
        put(Gate, Target | (uint64_t(8) << 16) | (uint64_t(0x8e) << 40));
    }
    if (DeliveryFailure == 1 || DeliveryFailure == 2)
      put(IDTBase + 14 * 16, get(IDTBase + 14 * 16) & ~(uint64_t(1) << 47));
    if (DeliveryFailure == 2)
      put(IDTBase + 8 * 16, get(IDTBase + 8 * 16) & ~(uint64_t(1) << 47));
    if (DeliveryFailure == 3)
      put(IDTBase + 14 * 16, get(IDTBase + 14 * 16) | (uint64_t(32) << 16));
    if (DeliveryFailure == 5 || DeliveryFailure == 6)
      put(KernelPT + 7 * Width, 0, Width);
    if (DeliveryFailure == 6)
      put(KernelPT + 6 * Width, 0, Width);
    if (DeliveryFailure == 7) {
      put(KernelPT + (Stack / 4096 - 1) * Width, 0, Width);
      put(IDTBase + 8 * 16, get(IDTBase + 8 * 16) | (uint64_t(1) << 32));
      put(TSSBase + 36, 0x1c0000);
    }
    // The oracle executes actual machine bytes, never candidate helper code.
    const std::array<uint8_t, 4> Bytes{
        uint8_t(Wide ? 0x48 : (Kind == MemoryAccessKind::Write ? 0x89 : 0x8b)),
        uint8_t(Wide ? (Kind == MemoryAccessKind::Write ? 0x89 : 0x8b) : 0x07),
        uint8_t(StackReference ? 0x45 : 0x07), 0};
    const unsigned CodeBytes = InitialEvent == 6   ? 2
                               : InitialEvent == 2 ? 1
                               : StackReference    ? 4
                               : Wide              ? 3
                                                   : 2;
    std::memcpy(RAM + Code, Bytes.data(), CodeBytes);
    if (InitialEvent == 6) {
      RAM[Code] = 0x0f;
      RAM[Code + 1] = 0x0b; // Independent original UD2, no operand access.
    } else if (InitialEvent == 2) {
      RAM[Code] = 0x90; // NMI is injected before this instruction executes.
    }
    const uint64_t Original = 0x9876543210fedc90ULL;
    for (unsigned I = 0; I != 8; ++I) {
      const uint64_t Destination = Offset + I < 4096
                                       ? DataPage + Offset + I
                                       : SecondDataPage + Offset + I - 4096;
      put(Destination, Original >> (I * 8), 1);
    }

    X86PagingState State;
    State.CR0 = 0x80000011 | (WriteProtect ? 0x10000 : 0);
    State.CR3 = 0x10000;
    State.CR4 = (Legacy ? 0 : 0x20) | (GetParam() == 5 ? 0x1000 : 0) |
                (Protection & 1 ? uint64_t(1) << 20 : 0) |
                (Protection & 2 ? uint64_t(1) << 21 : 0);
    State.EFER = (Wide ? 0x500 : 0) | (Legacy ? 0 : 0x800);
    State.RFlags = 2 | (AC ? uint64_t(1) << 18 : 0);
    State.PDPTE = {0x11001, 0x20001, 0, 0};
    auto Logical =
        llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
    Logical.Memory.push_back({0,
                              MemoryPermission::Read | MemoryPermission::Write,
                              0, std::vector<uint8_t>(RAM, RAM + MemorySize)});
    auto Memory =
        llvm::cantFail(X86PagedMemory::create(Logical, Features, State));
    const unsigned OperandBytes = Kind == MemoryAccessKind::Execute ? 1
                                  : Wide                            ? 8
                                                                    : 4;
    std::array<uint8_t, 8> Value;
    Value.fill(0xa5);
    const std::array<uint8_t, 8> Input{0x44, 0x33, 0x22, 0x11};
    const X86LinearAccess Access{Address, Kind, uint8_t(CPL), false,
                                 StackReference};
    auto Expected = [&]() -> llvm::Expected<X86MemoryAccessResult> {
      if (InitialEvent) {
        X86MemoryAccessResult R;
        R.Status = X86PageWalkStatus::Exception;
        R.Exception =
            X86MemoryException{uint8_t(InitialEvent), 0, std::nullopt};
        return R;
      }
      return Kind == MemoryAccessKind::Write
                 ? Memory->store(Access,
                                 llvm::ArrayRef(Input).take_front(OperandBytes))
                 : Memory->load(Access, llvm::MutableArrayRef(Value).take_front(
                                            OperandBytes));
    }();
    const bool HasExpected = bool(Expected);
    kvm_sregs Special = Initial;
    Special.cr0 = State.CR0;
    Special.cr3 = State.CR3;
    Special.cr4 = State.CR4;
    Special.efer = State.EFER;
    Special.cs = segment(CPL ? 27 : 8, true, Wide, CPL);
    Special.ds = Special.es = Special.ss =
        segment(CPL ? 35 : 16, false, Wide, CPL);
    Special.gdt.base = GDTBase;
    Special.gdt.limit = 0x3f;
    Special.idt.base = IDTBase;
    Special.idt.limit = 4095;
    Special.ldt.unusable = 1;
    Special.tr = {};
    Special.tr.base = TSSBase;
    Special.tr.limit = 0x67;
    Special.tr.selector = 40;
    Special.tr.type = 11;
    Special.tr.present = 1;
    const auto BeforeSpecial = Special;
    auto Flush = Special;
    Flush.cr3 = 0x90000;
    ASSERT_EQ(ioctl(CPU, KVM_SET_SREGS, &Flush), 0) << errno;
    ASSERT_EQ(ioctl(CPU, KVM_SET_SREGS, &Special), 0) << errno;
    kvm_vcpu_events Events{};
    Events.flags = KVM_VCPUEVENT_VALID_NMI_PENDING | KVM_VCPUEVENT_VALID_SHADOW;
    ASSERT_EQ(ioctl(CPU, KVM_SET_VCPU_EVENTS, &Events), 0);
    kvm_regs Registers{};
    Registers.rip = Kind == MemoryAccessKind::Execute ? Address : Code;
    Registers.rsp = CPL ? UserStack : KernelSP;
    Registers.rflags = State.RFlags;
    Registers.rdi = Address;
    Registers.rbp = Address;
    Registers.rax = 0x11223344;
    const auto BeforeRegisters = Registers;
    ASSERT_EQ(ioctl(CPU, KVM_SET_REGS, &Registers), 0);
    kvm_guest_debug Debug{};
    Debug.control = KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_USE_HW_BP;
    Debug.arch.debugreg[0] =
        Kind == MemoryAccessKind::Execute ? Address + 1 : Code + CodeBytes;
    Debug.arch.debugreg[1] = Handler + (InitialEvent ? InitialEvent : 14) * 16;
    Debug.arch.debugreg[2] = Handler + (DeliveryFailure ? 8 : 13) * 16;
    Debug.arch.debugreg[3] = Handler + 12 * 16;
    Debug.arch.debugreg[7] = 0x455;
    ASSERT_EQ(ioctl(CPU, KVM_SET_GUEST_DEBUG, &Debug), 0);
    if (InitialEvent == 2)
      ASSERT_EQ(ioctl(CPU, KVM_NMI, 0), 0);
    ASSERT_EQ(ioctl(CPU, KVM_RUN, 0), 0) << errno;
    const bool Shutdown = DeliveryFailure == 2 || DeliveryFailure == 6;
    ASSERT_EQ(Run->exit_reason,
              unsigned(Shutdown ? KVM_EXIT_SHUTDOWN : KVM_EXIT_DEBUG));
    ASSERT_EQ(ioctl(CPU, KVM_GET_REGS, &Registers), 0);
    ASSERT_EQ(ioctl(CPU, KVM_GET_SREGS, &Special), 0);
    ASSERT_TRUE(bool(Expected))
        << llvm::toString(Expected.takeError())
        << " native rip=" << Registers.rip << " cr2=" << Special.cr2
        << " rax=" << Registers.rax << " stack=" << get(Registers.rsp);
    ASSERT_NE(Expected->Status, X86PageWalkStatus::BackingFault);
    if (Expected->Status == X86PageWalkStatus::Exception) {
      ASSERT_TRUE(Expected->Exception);
      if (!DeliveryFailure) {
        EXPECT_EQ(Registers.rip, Handler + Expected->Exception->Vector * 16);
        EXPECT_EQ(get(Registers.rsp, Wide ? 8 : 4),
                  InitialEvent ? Code : Expected->Exception->ErrorCode);
        if (Expected->Exception->CR2)
          EXPECT_EQ(Special.cr2, *Expected->Exception->CR2);
      }
      // Faulting ordinary instructions publish neither their result nor data.
      EXPECT_EQ(Registers.rax, 0x11223344u);
      if (Kind != MemoryAccessKind::Write)
        for (uint8_t Byte : Value)
          EXPECT_EQ(Byte, 0xa5);
      if (Wide) {
        X64ContinuationStateV6 Explicit;
        auto &Core = Explicit.Core.Core.Core.Core;
        Core.PC = BeforeRegisters.rip;
        Core.RFlags = BeforeRegisters.rflags;
        Core.GPR[0] = BeforeRegisters.rax;
        Core.GPR[4] = BeforeRegisters.rsp;
        Core.GPR[7] = BeforeRegisters.rdi;
        const auto Segment = [](const kvm_segment &S) {
          return X64ContinuationSegment{
              S.base, S.limit, S.selector,
              uint16_t(S.type | S.s << 4 | S.dpl << 5 | S.present << 7 |
                       S.avl << 12 | S.l << 13 | S.db << 14 | S.g << 15)};
        };
        Explicit.Core.CS = Segment(BeforeSpecial.cs);
        Explicit.Core.SS = Segment(BeforeSpecial.ss);
        Explicit.TR = Segment(BeforeSpecial.tr);
        Explicit.GDTR = {BeforeSpecial.gdt.base, BeforeSpecial.gdt.limit};
        Explicit.IDTR = {BeforeSpecial.idt.base, BeforeSpecial.idt.limit};
        Explicit.Unusable = 64;
        X64ContinuationEventV1 Event;
        Event.SavedPC = BeforeRegisters.rip;
        Event.Kind = InitialEvent == 2 ? X64ContinuationEventKind::NMI
                                       : X64ContinuationEventKind::Fault;
        Event.Vector = Expected->Exception->Vector;
        Event.ErrorCode = Expected->Exception->ErrorCode;
        Event.FaultAddress = Expected->Exception->CR2.value_or(0);
        auto Delivery = deliverX64PagedEvent(Explicit, *Memory, Event, 0xffff);
        ASSERT_TRUE(bool(Delivery)) << llvm::toString(Delivery.takeError());
        ASSERT_EQ(Delivery->Status, Shutdown ? X64PagedEventStatus::Shutdown
                                             : X64PagedEventStatus::Delivered);
        EXPECT_EQ(Explicit.Core.Core.CR2, Special.cr2);
        kvm_vcpu_events AfterEvents{};
        ASSERT_EQ(ioctl(CPU, KVM_GET_VCPU_EVENTS, &AfterEvents), 0);
        EXPECT_EQ(Explicit.NMIBlocked, AfterEvents.nmi.masked);
        if (!Shutdown) {
          EXPECT_EQ(Delivery->DeliveredVector,
                    DeliveryFailure ? 8 : Expected->Exception->Vector);
          EXPECT_EQ(Core.PC, Registers.rip);
          EXPECT_EQ(Core.GPR[4], Registers.rsp);
          EXPECT_EQ(Core.RFlags, Registers.rflags);
          EXPECT_EQ(Explicit.Core.CS.Selector, Special.cs.selector);
          EXPECT_EQ(Explicit.Core.CS.Attributes,
                    Segment(Special.cs).Attributes);
          EXPECT_EQ(Explicit.Core.SS.Selector, Special.ss.selector);
          if (DeliveryFailure)
            EXPECT_EQ(get(Registers.rsp), 0u);
        }
        // #DF's saved program state is architecturally undefined. Ordinary
        // #PF entry still compares the complete stack page byte for byte.
        if (!DeliveryFailure) {
          std::array<uint8_t, 4096> Frame;
          ASSERT_EQ(
              Memory->physicalMemory().loadBytes(Stack - 4096, Frame).Status,
              GuestMemoryAccessStatus::Completed);
          EXPECT_EQ(std::memcmp(Frame.data(), RAM + Stack - 4096, Frame.size()),
                    0);
        } else if (DeliveryFailure == 7) {
          for (uint64_t Address : {Stack, Stack + 8})
            EXPECT_EQ(Memory->physicalMemory()
                          .loadScalar(Address, GuestScalarWidth::I64)
                          .Value,
                      get(Address));
        }
        ++EventComparisons;
        std::array<uint8_t, 4096> Descriptors;
        const uint64_t DescriptorPage = WrappingTable == 2 ? 0 : 0x5000;
        ASSERT_EQ(Memory->physicalMemory()
                      .loadBytes(DescriptorPage, Descriptors)
                      .Status,
                  GuestMemoryAccessStatus::Completed);
        EXPECT_EQ(std::memcmp(Descriptors.data(), RAM + DescriptorPage,
                              Descriptors.size()),
                  0);
      }
    } else {
      EXPECT_EQ(Registers.rip, Kind == MemoryAccessKind::Execute
                                   ? Address + 1
                                   : Code + CodeBytes);
      if (Kind == MemoryAccessKind::Read)
        EXPECT_EQ(Registers.rax, Wide ? 0x9876543210fedc90ULL : 0x10fedc90u);
      if (Kind != MemoryAccessKind::Write)
        for (unsigned I = 0; I != OperandBytes; ++I)
          EXPECT_EQ(Value[I], uint8_t(Original >> (I * 8)));
      // A/D changes on successful accesses are required. Fault-time speculative
      // A updates are allowed to differ and are intentionally not equated.
      for (uint64_t Entry : Entries) {
        const auto Software = Memory->physicalMemory().loadScalar(
            Entry, Legacy ? GuestScalarWidth::I32 : GuestScalarWidth::I64);
        EXPECT_EQ(get(Entry, Width), Software.Value);
      }
    }
    for (uint64_t PhysicalPage :
         {DataPage, SecondDataPage, WrappedPage, uint64_t(0)}) {
      std::array<uint8_t, 4096> Bytes;
      ASSERT_EQ(Memory->physicalMemory().loadBytes(PhysicalPage, Bytes).Status,
                GuestMemoryAccessStatus::Completed);
      EXPECT_EQ(std::memcmp(Bytes.data(), RAM + PhysicalPage, Bytes.size()), 0);
    }
  }
};

TEST_P(X86PagingNativeTest, OriginalInstructionsMatchPagingFaultsAndCommit) {
  const unsigned Levels = GetParam() >= 4 ? GetParam() : 2;
  unsigned Comparisons = 0;
  for (unsigned Level = 0; Level != Levels; ++Level)
    for (const auto &[Clear, Set] :
         std::array<std::pair<uint64_t, uint64_t>, 5>{
             {{0, 0},
              {1, 0},
              {2, 0},
              {4, 0},
              {0, GetParam() == 2 ? 0 : uint64_t(1) << 63}}})
      for (MemoryAccessKind Kind :
           {MemoryAccessKind::Read, MemoryAccessKind::Write,
            MemoryAccessKind::Execute})
        for (unsigned CPL : {0u, 3u})
          for (bool WP : {false, true}) {
            ASSERT_NO_FATAL_FAILURE(
                compare(Level, Clear, Set, Kind, CPL, WP, 0, false));
            ++Comparisons;
          }
  for (unsigned Protection : {1u, 2u, 3u}) {
    if (((Protection & 1) && !Features.SMEP) ||
        ((Protection & 2) && !Features.SMAP))
      continue;
    for (MemoryAccessKind Kind :
         {MemoryAccessKind::Read, MemoryAccessKind::Write,
          MemoryAccessKind::Execute})
      for (unsigned CPL : {0u, 3u})
        for (bool AC : {false, true}) {
          ASSERT_NO_FATAL_FAILURE(
              compare(Levels, 0, 0, Kind, CPL, true, Protection, AC));
          ++Comparisons;
        }
  }
  for (unsigned Prefix = 1; Prefix != (GetParam() >= 4 ? 8u : 4u); ++Prefix)
    for (bool Present : {false, true})
      for (MemoryAccessKind Kind :
           {MemoryAccessKind::Read, MemoryAccessKind::Write})
        for (unsigned CPL : {0u, 3u}) {
          ASSERT_NO_FATAL_FAILURE(compare(Levels, 0, 0, Kind, CPL, true, 0,
                                          false, Prefix, Present));
          ++Comparisons;
        }
  if (GetParam() != 2)
    for (unsigned Level = 0; Level != Levels; ++Level)
      for (uint64_t Reserved :
           {uint64_t(1) << Features.PhysicalAddressBits, uint64_t(1) << 62})
        for (MemoryAccessKind Kind :
             {MemoryAccessKind::Read, MemoryAccessKind::Write,
              MemoryAccessKind::Execute}) {
          ASSERT_NO_FATAL_FAILURE(
              compare(Level, 0, Reserved, Kind, 3, true, 0, false));
          ++Comparisons;
        }
  if (GetParam() >= 4)
    for (unsigned Failure : {1u, 2u, 3u, 5u, 6u, 7u})
      for (unsigned CPL : {0u, 3u})
        for (MemoryAccessKind Kind :
             {MemoryAccessKind::Read, MemoryAccessKind::Write,
              MemoryAccessKind::Execute}) {
          ASSERT_NO_FATAL_FAILURE(compare(Levels - 1, 1, 0, Kind, CPL, true, 0,
                                          false, 0, true, Failure));
          ++Comparisons;
        }
  if (GetParam() >= 4)
    for (unsigned Boundary : {1u, 2u})
      for (unsigned Prefix = 1; Prefix != 8; ++Prefix)
        for (bool StackReference : {false, true})
          for (bool Present : {true, false})
            for (MemoryAccessKind Kind :
                 {MemoryAccessKind::Read, MemoryAccessKind::Write}) {
              ASSERT_NO_FATAL_FAILURE(
                  compare(Levels - 1, Present ? 0 : 1, 0, Kind, 0, true, 0,
                          false, Prefix, true, 0, Boundary, StackReference));
              ++Comparisons;
            }
  if (GetParam() >= 4)
    for (unsigned InitialEvent : {2u, 6u})
      for (unsigned Failure : {0u, 7u})
        for (unsigned CPL : {0u, 3u}) {
          ASSERT_NO_FATAL_FAILURE(compare(Levels, 0, 0, MemoryAccessKind::Read,
                                          CPL, true, 0, false, 0, true, Failure,
                                          false, false, InitialEvent));
          ++Comparisons;
        }
  if (GetParam() >= 4)
    for (unsigned Table : {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u})
      for (unsigned CPL : {0u, 3u}) {
        ASSERT_NO_FATAL_FAILURE(
            compare(Levels - 1, 1, 0, MemoryAccessKind::Read, CPL, true, 0,
                    false, 0, true, 0, false, false, 0, Table));
        ++Comparisons;
      }
  RecordProperty("cpu_comparisons", Comparisons);
  RecordProperty("event_cpu_comparisons", EventComparisons);
}

#include "X86LegacyEventNativeTests.inc"
#include "X86LegacyReturnNativeTests.inc"
#include "X86LegacyScalarNativeTests.inc"
#include "X86LegacySegmentLoadNativeTests.inc"
#include "X86LegacySegmentNativeTests.inc"
#include "X86LegacyStepNativeTests.inc"

INSTANTIATE_TEST_SUITE_P(PagingModes, X86PagingNativeTest,
                         testing::Values(2u, 3u, 4u, 5u));

#else
TEST(X86PagingNativeTest, RequiresLinuxX64KVM) {
  GTEST_SKIP() << "Native paging oracle requires Linux x64 KVM";
}
#endif

} // namespace
