//===- X64PagedDescriptorBoundaryFixture.inc - Descriptor inputs --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared explicit descriptor-range inputs for independent code64 owners.
//===----------------------------------------------------------------------===//

#if defined(__linux__) && defined(__x86_64__)
class X64PagedDescriptorBoundaryFixture : public X64PagedExecutionTest {
protected:
  static constexpr uint64_t InitialCR2 = 0x123456789abcdef0ULL;

  static uint64_t descriptorAddress(unsigned Case) {
    switch (Case) {
    case 0:
      return 0x00007ffffffffff8ULL;
    case 1:
      return 0x0000800000000000ULL;
    case 2:
    case 3:
      return 0x00007ffffffffffcULL;
    case 4:
      return 0xffff800000008020ULL;
    case 5:
      return 0xfffffffffffffff8ULL;
    case 8:
      return 0xffff800000000020ULL;
    case 9:
      return 0;
    default:
      return 0xfffffffffffffffcULL;
    }
  }

  static bool faults(unsigned Case) {
    return (Case >= 1 && Case <= 4) || Case == 7 || Case >= 11;
  }

  static bool pageFault(unsigned Case) {
    return Case == 4 || Case == 7 || Case >= 11;
  }

  static uint32_t error(unsigned Case, bool TailGPZero) {
    if (Case >= 11)
      return 3;
    return Case == 1 || ((Case == 2 || Case == 3) && !TailGPZero) ? 0x24 : 0;
  }

  static uint64_t cr2(unsigned Case) {
    return Case == 12        ? 0
           : pageFault(Case) ? descriptorAddress(Case)
                             : InitialCR2;
  }

  void descriptorBoundary(const std::vector<uint8_t> &Code, unsigned Case,
                          uint64_t Descriptor) {
    reset(Code);
    // The original oracle executes a handler HLT; the model stops at entry.
    // Pre-access that executable page in both inputs so the exit instrument
    // adds no guest PTE effect to the complete-RAM comparison.
    put(0x13020, 0x4027);
    auto &Core = State.Core.Core.Core.Core;
    State.Core.Core.CR2 = InitialCR2;
    State.DS = State.ES = State.Core.SS;
    State.FS = State.GS = {UINT32_MAX, 16, 0xc093};
    Core.FSBase = 0xffff800000022000ULL;
    const auto Address = descriptorAddress(Case);
    State.LDTR = {Address - 0x20, 0x3f, 0x30, 0x82};
    State.Unusable &= ~64;
    if (Case == 0 || Case == 2 || Case == 3) {
      put(0x107f8, 0x14003);
      put(0x14ff8, 0x15003);
      put(0x15ff8, 0x16003);
      put(0x16ff8, Case == 3 ? 0x17002 : 0x17003);
    } else if (Case == 8) {
      put(0x10800, 0x14003);
      put(0x14000, 0x15003);
      put(0x15000, 0x16003);
      put(0x16000, 0x17003);
    } else if (Case >= 5 && Case != 9) {
      // Faulting walks may update ancestor A bits differently on the qualified
      // cores. Pre-access those ancestors for the absent-first-page case;
      // compare every output byte without masking an observed difference.
      const uint64_t Accessed = Case == 7 ? 0x20 : 0;
      put(0x10ff8, 0x14003 | Accessed);
      put(0x14ff8, 0x15003 | Accessed);
      put(0x15ff8, 0x16003 | Accessed);
      put(0x16ff8, Case == 7 ? 0x17002 : Case == 11 ? 0x17001 : 0x17003);
      if (Case == 12)
        put(0x13000, 1);
    }
    if (Case >= 10)
      Descriptor &= ~(uint64_t(1) << 40);
    if (Case != 1 && Case != 4) {
      const uint64_t Offset = Address & 0xfff;
      for (unsigned I = 0; I != 8; ++I) {
        const auto Physical = Case == 9             ? I
                              : Offset + I < 0x1000 ? 0x17000 + Offset + I
                                                    : Offset + I - 0x1000;
        RAM.Memory.front().Bytes[Physical] = Descriptor >> (I * 8);
      }
    }
  }

  static std::vector<std::string> compilers() {
    std::vector<std::string> Result{NEVERD_TEST_CLANG};
    if (const char *NeverC = std::getenv("NEVERD_TEST_NEVERC"))
      Result.emplace_back(NeverC);
    return Result;
  }

  std::unique_ptr<X86PagedMemory> without(unsigned Page) {
    auto Snapshot = RAM;
    const auto &Bytes = RAM.Memory.front().Bytes;
    Snapshot.Memory.clear();
    Snapshot.Memory.push_back({0,
                               MemoryPermission::Read | MemoryPermission::Write,
                               0,
                               {Bytes.begin(), Bytes.begin() + Page}});
    Snapshot.Memory.push_back({Page + 4096,
                               MemoryPermission::Read | MemoryPermission::Write,
                               0,
                               {Bytes.begin() + Page + 4096, Bytes.end()}});
    return llvm::cantFail(X86PagedMemory::create(Snapshot, Features, Controls));
  }

  std::unique_ptr<X86PagedMemory> repair(X86PagedMemory &Current,
                                         unsigned Page) {
    auto Snapshot = RAM;
    Snapshot.Memory = Current.physicalMemory().snapshotMemoryRegions();
    const auto &Bytes = RAM.Memory.front().Bytes;
    Snapshot.Memory.push_back(
        {Page,
         MemoryPermission::Read | MemoryPermission::Write,
         0,
         {Bytes.begin() + Page, Bytes.begin() + Page + 4096}});
    return llvm::cantFail(X86PagedMemory::create(Snapshot, Current.features(),
                                                 Current.controls()));
  }

  static std::vector<uint8_t> bytes(X86PagedMemory &Current) {
    std::vector<uint8_t> Result(0x40000);
    for (const auto &Region : Current.physicalMemory().snapshotMemoryRegions())
      std::copy(Region.Bytes.begin(), Region.Bytes.end(),
                Result.begin() + Region.Address);
    return Result;
  }

  static void compareRAM(X86PagedMemory &Current,
                         llvm::ArrayRef<uint8_t> Expected) {
    const auto Actual = bytes(Current);
    ASSERT_EQ(Actual.size(), Expected.size());
    const auto Difference =
        std::mismatch(Actual.begin(), Actual.end(), Expected.begin());
    EXPECT_EQ(Difference.first, Actual.end())
        << "RAM differs at " << (Difference.first - Actual.begin());
  }
};
#endif
