//===- ExecutionPolicyTests.cpp - Instruction policy tests ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Environment-dependent implicit memory accesses must stop before observing
/// unmodeled CPU state; ignored prefixes must not reject ordinary instructions.
///
//===----------------------------------------------------------------------===//

#include "X64ExecutionPolicy.h"
#include "gtest/gtest.h"
#include "unicorn/UnicornBackend.h"

#include "neverd/emulation/DriverProfile.h"

#include <array>

namespace neverd::emulation {
namespace {
class DriverExecutionPolicy : public ::testing::Test {
protected:
  X64ExecutionPolicy Policy;
  void SetUp() override {
    auto Error = Policy.initialize();
    ASSERT_FALSE(static_cast<bool>(Error)) << llvm::toString(std::move(Error));
  }
  void rejectsThreadAccess(llvm::ArrayRef<uint8_t> Bytes) {
    auto Error = Policy.validate(Bytes, 0x1000);
    ASSERT_TRUE(static_cast<bool>(Error));
    EXPECT_NE(llvm::toString(std::move(Error)).find("FS/GS"),
              std::string::npos);
  }
  void accepts(llvm::ArrayRef<uint8_t> Bytes) {
    auto Error = Policy.validate(Bytes, 0x1000);
    EXPECT_FALSE(static_cast<bool>(Error)) << llvm::toString(std::move(Error));
  }
  void rejectsEnvironment(llvm::ArrayRef<uint8_t> Bytes,
                          llvm::StringRef Mnemonic) {
    auto Error = Policy.validate(Bytes, 0x1000);
    ASSERT_TRUE(static_cast<bool>(Error));
    EXPECT_EQ(llvm::toString(std::move(Error)),
              "unmodeled CPU environment instruction: " + Mnemonic.str());
  }
};

TEST_F(DriverExecutionPolicy, RejectsFSAndGSImplicitXLATMemory) {
  rejectsThreadAccess({0x64, 0xd7});
  rejectsThreadAccess({0x65, 0xd7});
  rejectsThreadAccess({0x67, 0x65, 0xd7});
}

TEST_F(DriverExecutionPolicy, KeepsOrdinaryXLATAndIgnoredSegmentPrefixes) {
  accepts({0xd7});
  accepts({0x3e, 0xd7});
  accepts({0x65, 0x90});             // NOP does not access GS memory.
  accepts({0x64, 0x48, 0x89, 0xd8}); // Register MOV ignores the prefix.
}

TEST_F(DriverExecutionPolicy, RejectsExplicitAndStringThreadMemory) {
  rejectsThreadAccess({0x65, 0x48, 0x8b, 0x00}); // MOV rax, gs:[rax].
  rejectsThreadAccess({0x64, 0xac});             // LODS byte through FS.
}

TEST_F(DriverExecutionPolicy,
       RejectsFarControlFlowThatChangesImplicitSegments) {
  rejectsEnvironment({0x48, 0xff, 0x18}, "lcall");
  rejectsEnvironment({0x48, 0xff, 0x28}, "ljmp");
  rejectsEnvironment({0xcb}, "retf");
  rejectsEnvironment({0x48, 0xcb}, "retfq");
  rejectsEnvironment({0xca, 0x10, 0x00}, "retf");
  rejectsEnvironment({0x48, 0xca, 0x10, 0x00}, "retfq");
  accepts({0xff, 0x10}); // Near indirect CALL stays in this code segment.
  accepts({0xff, 0x20}); // Near indirect JMP.
  accepts({0xc3});       // Near RET.
}

TEST_F(DriverExecutionPolicy,
       RejectsUnprivilegedMSRFormsAcrossEncodingFamilies) {
  rejectsEnvironment({0xf2, 0x45, 0x0f, 0x38, 0xf8, 0xc7}, "urdmsr");
  rejectsEnvironment({0xf3, 0x45, 0x0f, 0x38, 0xf8, 0xc7}, "uwrmsr");
  rejectsEnvironment({0xc4, 0x07, 0x7b, 0xf8, 0xc7, 0x00, 0x1b, 0x00, 0x00},
                     "urdmsr");
  rejectsEnvironment({0xc4, 0x07, 0x7a, 0xf8, 0xc7, 0x01, 0x1b, 0x00, 0x00},
                     "uwrmsr");
  rejectsEnvironment(
      {0x62, 0xff, 0x7f, 0x08, 0xf8, 0xc3, 0x00, 0x1b, 0x00, 0x00}, "urdmsr");
  rejectsEnvironment(
      {0x62, 0xff, 0x7e, 0x08, 0xf8, 0xc4, 0x01, 0x1b, 0x00, 0x00}, "uwrmsr");
  rejectsEnvironment({0x62, 0xec, 0x7f, 0x08, 0xf8, 0xf5}, "urdmsr");
  rejectsEnvironment({0x62, 0xcc, 0x7e, 0x08, 0xf8, 0xf8}, "uwrmsr");
}

TEST_F(DriverExecutionPolicy, ImplicitThreadReadStopsBeforeUnicornReadsMemory) {
  auto Backend = UnicornBackend::create(4 * profile::PageSize);
  ASSERT_TRUE(static_cast<bool>(Backend))
      << llvm::toString(Backend.takeError());
  auto &CPU = **Backend;
  auto RequireSuccess = [](llvm::Error Error) {
    EXPECT_FALSE(static_cast<bool>(Error)) << llvm::toString(std::move(Error));
  };
  constexpr uint64_t CodeAddress = 0x1000;
  constexpr uint64_t DataAddress = 0x2000;
  RequireSuccess(
      CPU.map(CodeAddress, profile::PageSize, Read | Write | Execute));
  RequireSuccess(CPU.map(DataAddress, profile::PageSize, Read | Write));
  // mov ebx, 0x2000; xor eax, eax; gs xlatb; int3.
  const std::array<uint8_t, 10> Code = {0xbb, 0x00, 0x20, 0x00, 0x00,
                                        0x31, 0xc0, 0x65, 0xd7, 0xcc};
  RequireSuccess(CPU.write(CodeAddress, Code));
  RequireSuccess(CPU.writeInteger(DataAddress, 0x7f, 1));
  bool Rejected = false;
  unsigned DataReads = 0;
  BackendHooks Hooks;
  Hooks.Instruction = [&](uint64_t PC, uint32_t Size) {
    std::array<uint8_t, profile::MaxInstructionSize> Bytes{};
    if (Size > Bytes.size()) {
      ADD_FAILURE() << "invalid instruction size";
      CPU.stop();
      return;
    }
    auto Target = llvm::MutableArrayRef<uint8_t>(Bytes.data(), Size);
    if (auto Error = CPU.fetch(PC, Target)) {
      ADD_FAILURE() << llvm::toString(std::move(Error));
      CPU.stop();
      return;
    }
    if (auto Error = Policy.validate(Target, PC)) {
      EXPECT_EQ(PC, CodeAddress + 7);
      EXPECT_NE(llvm::toString(std::move(Error)).find("FS/GS"),
                std::string::npos);
      Rejected = true;
      CPU.stop();
    }
  };
  Hooks.Read = [&](uint64_t Address, uint32_t) {
    if (Address == DataAddress)
      ++DataReads;
  };
  Hooks.Interrupt = [&](uint32_t) {
    ADD_FAILURE() << "execution continued past rejected instruction";
    CPU.stop();
  };
  RequireSuccess(CPU.installHooks(std::move(Hooks)));
  RequireSuccess(CPU.run(CodeAddress, 1000000));
  EXPECT_TRUE(Rejected);
  EXPECT_EQ(DataReads, 0u);
  auto AX = CPU.reg(X64Register::AX);
  ASSERT_TRUE(static_cast<bool>(AX)) << llvm::toString(AX.takeError());
  EXPECT_EQ(*AX, 0u);
}

TEST(DriverUnicornAddressSpace,
     CanonicalKernelAddressesPreserveAccessesAndPermissions) {
  constexpr uint64_t CodeAddress = 0xfffff80010000000;
  constexpr uint64_t DataAddress = CodeAddress + profile::PageSize;
  constexpr uint64_t InitialValue = 0x1234abcd9876ef01;
  // mov rcx, [rax]; inc rcx; mov [rax], rcx; int3.
  const std::array<uint8_t, 10> Code = {0x48, 0x8b, 0x08, 0x48, 0xff,
                                        0xc1, 0x48, 0x89, 0x08, 0xcc};
  for (bool WritableData : {true, false}) {
    SCOPED_TRACE(WritableData ? "writable data" : "read-only data");
    auto Backend = UnicornBackend::create(2 * profile::PageSize);
    ASSERT_TRUE(static_cast<bool>(Backend))
        << llvm::toString(Backend.takeError());
    auto &CPU = **Backend;
    ASSERT_EQ(llvm::toString(CPU.map(CodeAddress, profile::PageSize,
                                     Read | Write | Execute)),
              "");
    ASSERT_EQ(
        llvm::toString(CPU.map(DataAddress, profile::PageSize, Read | Write)),
        "");
    ASSERT_EQ(llvm::toString(CPU.write(CodeAddress, Code)), "");
    ASSERT_EQ(llvm::toString(CPU.writeInteger(DataAddress, InitialValue, 8)),
              "");
    ASSERT_EQ(llvm::toString(
                  CPU.protect(CodeAddress, profile::PageSize, Read | Execute)),
              "");
    if (!WritableData)
      ASSERT_EQ(
          llvm::toString(CPU.protect(DataAddress, profile::PageSize, Read)),
          "");
    ASSERT_EQ(llvm::toString(CPU.setReg(X64Register::AX, DataAddress)), "");

    unsigned Instructions = 0;
    unsigned Reads = 0;
    bool Returned = false;
    bool Faulted = false;
    BackendHooks Hooks;
    Hooks.Instruction = [&](uint64_t Address, uint32_t) {
      EXPECT_GE(Address, CodeAddress);
      EXPECT_LT(Address, CodeAddress + Code.size());
      ++Instructions;
    };
    Hooks.Read = [&](uint64_t Address, uint32_t Size) {
      EXPECT_EQ(Address, DataAddress);
      EXPECT_EQ(Size, 8u);
      ++Reads;
    };
    Hooks.Write = [&](uint64_t Address, uint32_t Size, uint64_t Value) {
      EXPECT_EQ(Address, DataAddress);
      EXPECT_EQ(Size, 8u);
      EXPECT_EQ(Value, InitialValue + 1);
    };
    Hooks.Fault = [&](uint64_t Address, uint32_t Size, const char *Access) {
      EXPECT_EQ(Address, DataAddress);
      EXPECT_EQ(Size, 8u);
      EXPECT_STREQ(Access, "write");
      Faulted = true;
    };
    Hooks.Interrupt = [&](uint32_t Number) {
      EXPECT_EQ(Number, 3u);
      Returned = true;
      CPU.stop();
    };
    ASSERT_EQ(llvm::toString(CPU.installHooks(std::move(Hooks))), "");
    auto Error = CPU.run(CodeAddress, 1000000);
    if (WritableData) {
      EXPECT_FALSE(static_cast<bool>(Error))
          << llvm::toString(std::move(Error));
    } else {
      ASSERT_TRUE(static_cast<bool>(Error));
      EXPECT_NE(llvm::toString(std::move(Error)).find("UC_ERR_WRITE_PROT"),
                std::string::npos);
    }
    EXPECT_EQ(Instructions, WritableData ? 4u : 3u);
    EXPECT_EQ(Reads, 1u);
    EXPECT_EQ(Returned, WritableData);
    EXPECT_EQ(Faulted, !WritableData);
    EXPECT_EQ(CPU.hasMemoryFault(), !WritableData);
    EXPECT_FALSE(CPU.timedOut());
    auto Value = CPU.readInteger(DataAddress, 8);
    ASSERT_TRUE(static_cast<bool>(Value)) << llvm::toString(Value.takeError());
    EXPECT_EQ(*Value, InitialValue + (WritableData ? 1 : 0));
  }
}
} // namespace
} // namespace neverd::emulation
