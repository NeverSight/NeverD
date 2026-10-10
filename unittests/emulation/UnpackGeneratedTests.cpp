//===- UnpackGeneratedTests.cpp - Observation on every instruction set ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The rules that establish an entry belong to no protector, container
/// layout or instruction set. These cases check them against a program that
/// is packed here, by the test, from a file whose every byte is known: the
/// program as it was linked.
///
//===----------------------------------------------------------------------===//
#include "HvfTestPolicy.h"
#include "UnpackGeneratedTestSupport.h"
#include "WindowsNativeFailure.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessObserver.h"
#include "neverd/emulation/ProcessSession.h"
#include "neverd/unpack/Unpack.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/Program.h"

#if defined(_WIN32) && defined(_M_X64)
#include <intrin.h>
#endif

namespace neverd::unpack {
namespace {
using namespace emulation;
using test::differingBytes;
using test::Image;
namespace generated = test::generated;
using namespace generated;

struct Profile {
  const char *Name;
  ExecutionBackendKind Backend;
  GuestArchitecture ISA;
  ExecutionContract Contract;
  const char *Directory;
};
constexpr Profile Profiles[] = {
#define NEVERD_USER_PROFILE(Name, Backend, ISA, Contract)                      \
  {#Name, ExecutionBackendKind::Backend, GuestArchitecture::ISA,               \
   ExecutionContract::Contract, ISA##Dir},
#include "UserExecutionCases.def"
#undef NEVERD_USER_PROFILE
    {"UnicornDirectX64", ExecutionBackendKind::Unicorn, GuestArchitecture::X64,
     ExecutionContract::DirectUserX64, X64Dir},
    {"KvmDirectX64", ExecutionBackendKind::KVM, GuestArchitecture::X64,
     ExecutionContract::DirectUserX64, X64Dir},
    {"WhpDirectX64", ExecutionBackendKind::WHP, GuestArchitecture::X64,
     ExecutionContract::DirectUserX64, X64Dir},
};
void PrintTo(const Profile &P, std::ostream *OS) { *OS << P.Name; }

enum class SamePageCode {
  GeneratedJump,
  RewrittenCallback,
  MixedInstructionLoop,
  AdjacentData,
  ChangedOperand,
  ServiceWrite
};

class UnpackGenerated : public testing::TestWithParam<Profile> {
protected:
  void SetUp() override {
#ifndef NEVERD_UNPACK_GENERATED_FIXTURE_DIR
    if (requireHvf(GetParam().Backend, GetParam().ISA))
      FAIL() << MissingTools;
    GTEST_SKIP() << MissingTools;
#else
    const auto &P = GetParam();
    ExecutionConfiguration Config;
    Config.Backend = P.Backend;
    Config.Architecture = P.ISA;
    Config.Contract = P.Contract;
    auto Probe = probeExecutionBackend(Config);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available) {
      if (requireHvf(P.Backend, P.ISA))
        FAIL() << Probe->Reason;
      GTEST_SKIP() << Probe->Reason;
    }
    Original = test::readImage(
        std::filesystem::path(NEVERD_UNPACK_GENERATED_FIXTURE_DIR) /
        P.Directory / ProgramFile);
    ASSERT_FALSE(HasFailure());
    llvm::SmallString<128> Created;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(ScratchPrefix, Created));
    Scratch = Created.str().str();
    Options.Process.Backend = P.Backend;
    Options.Process.Contract = P.Contract;
    Options.Process.Windows.emplace().DeferUnmodeled = true;
    Options.Process.Limits.Instructions = InstructionLimit;
    Options.Process.Limits.TimeoutMicroseconds = TimeoutMicroseconds;
#endif
  }
  void useTLSHeapFixture() {
#ifdef NEVERD_UNPACK_GENERATED_FIXTURE_DIR
    Original = test::readImage(
        std::filesystem::path(NEVERD_UNPACK_GENERATED_FIXTURE_DIR) /
        GetParam().Directory / TLSHeapProgramFile);
    ASSERT_FALSE(HasFailure());
#endif
  }
  void TearDown() override {
    if (!Scratch.empty())
      std::filesystem::remove_all(Scratch);
  }

  void expectNativeWindows(const std::filesystem::path &Path, int Status) {
#if defined(_WIN32) && defined(_M_X64)
    if (GetParam().ISA != GuestArchitecture::X64)
      return;
    const auto Native = Path.string();
    std::string Diagnostic;
    bool Failed = false;
    const auto Actual = llvm::sys::ExecuteAndWait(
        Native, {Native}, std::nullopt, {}, 30, 0, &Diagnostic, &Failed);
    EXPECT_EQ(Actual, Status) << Diagnostic;
    EXPECT_FALSE(Failed) << Diagnostic;
    if (Actual != Status && !Failed)
      diagnoseNativeFailure(Path.c_str());
#else
    (void)Path;
    (void)Status;
#endif
  }

  /// Replace each import call with an independent helper. The linked file
  /// stays the oracle; only this copy carries the transformed call.
  bool mutateImportCalls(std::vector<uint8_t> &Bytes, uint32_t Mode) {
    using namespace llvm::support::endian;
    if (GetParam().ISA != GuestArchitecture::X64) {
      ADD_FAILURE() << X64CallShape;
      return false;
    }
    const auto *Program = Original.section(ProgramSection);
    if (!Program || Program->VirtualSize > Program->FileSize ||
        Program->FileOffset + Program->VirtualSize > Bytes.size()) {
      ADD_FAILURE() << MissingRecord;
      return false;
    }
    const char *Names[] = {"ExitProcess", "GetStdHandle", "WriteFile"};
    bool Seen[] = {false, false, false};
    for (uint32_t At = 0; At + 6 <= Program->VirtualSize; ++At) {
      uint8_t *Site = Bytes.data() + Program->FileOffset + At;
      if (Site[0] != 0xff || Site[1] != 0x15)
        continue;
      const int32_t Displacement = read32le(Site + 2);
      const uint64_t Next = Original.Base + Program->RVA + At + 6;
      const uint64_t Slot = uint64_t(int64_t(Next) + Displacement);
      const Image::Import *Match = nullptr;
      for (const auto &Import : Original.Imports)
        if (Original.Base + Import.Slot == Slot)
          Match = &Import;
      if (!Match)
        continue;
      if (!llvm::is_contained(Names, Match->Name))
        continue;
      const auto Tail = Original.Exports.find(
          Mode == ImpureCallMode && Match->Name == "GetStdHandle"
              ? "impure_tail_GetStdHandle"
              : (Mode == PaddedCallMode ? "call_" : "tail_") + Match->Name);
      if (Tail == Original.Exports.end()) {
        ADD_FAILURE() << Match->Name << MissingTail;
        return false;
      }
      const int64_t Relative = int64_t(Original.Base + Tail->second) -
                               int64_t(Next - (Mode == PaddedCallMode ? 1 : 0));
      if (Relative != int32_t(Relative)) {
        ADD_FAILURE() << TailOutOfRange;
        return false;
      }
      if (Mode == PaddedCallMode) {
        Site[0] = 0xe8;
        write32le(Site + 1, uint32_t(Relative));
        Site[5] = 0x0f;
      } else {
        Site[0] = 0x51;
        Site[1] = 0xe8;
        write32le(Site + 2, uint32_t(Relative));
      }
      for (size_t I = 0; I < sizeof Names / sizeof Names[0]; ++I)
        if (Match->Name == Names[I])
          Seen[I] = true;
      At += 5;
    }
    for (bool Found : Seen)
      if (!Found) {
        ADD_FAILURE() << NoImportCall;
        return false;
      }
    return true;
  }

  /// Do to the linked program what a packer does: keep its code only in a
  /// transformed copy and start at the loader that restores it.
  std::filesystem::path pack(uint32_t Mode) {
    using namespace llvm::support::endian;
    auto Bytes = Original.File;
    if ((Mode == MutatedMode || Mode == PaddedCallMode ||
         Mode == ImpureCallMode) &&
        !mutateImportCalls(Bytes, Mode))
      return {};
    if (Mode == OpaqueCallMode || Mode == ReboundOpaqueCallMode ||
        Mode == LateOpaqueCallMode) {
      const auto *Code = Original.section(ProgramSection);
      const uint64_t RVA =
          Mode == OpaqueCallMode
              ? Original.Exports.at("opaque_program") + OpaqueCallOffset
              : Original.Exports.at("late_program") + LateCallOffset;
      const uint64_t At = Code->FileOffset + RVA - Code->RVA;
      if (Bytes[At] != 0xff || Bytes[At + 1] != 0x15) {
        ADD_FAILURE() << "independent program has no opaque import call";
        return {};
      }
      const uint64_t Target = Original.Exports.at(
          Mode == OpaqueCallMode ? "call_SetUnhandledExceptionFilter"
                                 : "late_export_helper");
      Bytes[At] = 0xe8;
      write32le(Bytes.data() + At + 1, uint32_t(Target - (RVA + 5)));
      Bytes[At + 5] = 0x0f;
    }
    if ((Mode >= AddressMode && Mode <= UnresolvedAddressMode) ||
        Mode == PreviousPrefixAddressMode || Mode == CallOnlyAddressMode) {
      const auto *Code = Original.section(ProgramSection);
      const uint64_t RVA =
          Mode == PreviousPrefixAddressMode
              ? Original.Exports.at("address_previous_prefix") + 10
          : Mode == ChangingAddressMode || Mode == UnresolvedAddressMode
              ? Original.Exports.at("address_twice") + 10
              : Original.Exports.at("address_program") + 5;
      const uint64_t At = Code->FileOffset + RVA - Code->RVA;
      if (Bytes[At] != 0x48 || Bytes[At + 1] != 0x8b || Bytes[At + 2] != 0x1d) {
        ADD_FAILURE() << "independent program has no seven-byte import load";
        return {};
      }
      const uint64_t Target = Original.Exports.at(
          Mode == CallOnlyAddressMode ? "call_address_helper"
          : Mode == AddressMode || Mode == PreviousPrefixAddressMode
              ? "address_helper"
          : Mode == ChangingAddressMode   ? "changing_address_helper"
          : Mode == UnresolvedAddressMode ? "unresolved_address_helper"
          : Mode == ServiceAddressMode    ? "service_address_helper"
                                          : "impure_address_helper");
      if (Mode == CallOnlyAddressMode) {
        Bytes[At] = 0xe8;
        write32le(Bytes.data() + At + 1, uint32_t(Target - (RVA + 5)));
        Bytes[At + 5] = 0x0f;
        Bytes[At + 6] = 0x0b;
      } else {
        Bytes[At] = 0x5b;
        Bytes[At + 1] = 0xe8;
        write32le(Bytes.data() + At + 2, uint32_t(Target - (RVA + 6)));
        Bytes[At + 6] = 0xc3;
      }
    }
    if (Mode == ExtendedAddressMode || Mode == CompactAddressMode) {
      const auto *Code = Original.section(ProgramSection);
      for (unsigned Register = 8; Register != 16; ++Register) {
        const uint64_t RVA = Original.Exports.at("address_extended_program") +
                             ExtendedLoadOffset +
                             (Register - 8) * ExtendedLoadStride;
        const uint64_t At = Code->FileOffset + RVA - Code->RVA;
        if (Bytes[At] != 0x4c || Bytes[At + 1] != 0x8b ||
            Bytes[At + 2] != (0x05 | ((Register & 7) << 3)) ||
            Bytes[At + 7] != 0x90) {
          ADD_FAILURE() << "independent program has no extended import load";
          return {};
        }
        const auto Target = Original.Exports.at((Mode == CompactAddressMode
                                                     ? "compact_helper_r"
                                                     : "address_helper_r") +
                                                std::to_string(Register));
        Bytes[At] = 0x41;
        Bytes[At + 1] = 0x58 | (Register & 7);
        Bytes[At + 2] = 0xe8;
        write32le(Bytes.data() + At + 3, uint32_t(Target - (RVA + 7)));
        Bytes[At + 7] = Mode == CompactAddressMode ? 0x90 : 0xc3;
      }
    }
    Bytes = generated::pack(Original, std::move(Bytes), Mode);
    if (HasFailure())
      return {};
    const auto Path = Scratch / PackedFile;
    test::writeFile(Path, Bytes);
    return Path;
  }

  UnpackResult unpack(uint32_t Mode) {
    const auto Packed = pack(Mode);
    if (HasFailure())
      return {};
    auto Result = unpackFile(Packed, Options);
    if (!Result) {
      ADD_FAILURE() << llvm::toString(Result.takeError());
      return {};
    }
    return std::move(*Result);
  }

  ProcessResult run(const std::filesystem::path &Path) {
    auto Result =
        emulateProcess(Path, ProcessProfile::WindowsPE64, Options.Process);
    if (!Result) {
      ADD_FAILURE() << llvm::toString(Result.takeError());
      return {};
    }
    return std::move(*Result);
  }
  ProcessResult runOriginal(uint32_t Mode = 0) {
    const auto Path = Scratch / ProgramFile;
    auto Bytes = Original.File;
    const auto *Record = Original.section(PackSection);
    if (!Record) {
      ADD_FAILURE() << MissingRecord;
      return {};
    }
    llvm::support::endian::write32le(
        Bytes.data() + Record->FileOffset + ModeOffset, Mode);
    test::writeFile(Path, Bytes);
    return run(Path);
  }

  /// The rebuilt file holds the program as it was linked: every section but
  /// the pack record, which the linked file leaves empty.
  void expectOriginalProgram(const UnpackResult &Result, uint32_t Mode = 0) {
    const Image Rebuilt = test::readImage(Result.Image);
    ASSERT_FALSE(HasFailure());
    EXPECT_EQ(Rebuilt.Entry, Original.Entry);
    EXPECT_EQ(Rebuilt.Base, Original.Base);
    for (const auto &S : Original.Sections)
      if (S.Name != PackSection)
        EXPECT_EQ(differingBytes(Original, Rebuilt, S), 0u) << S.Name;
    EXPECT_TRUE(std::includes(Rebuilt.Imports.begin(), Rebuilt.Imports.end(),
                              Original.Imports.begin(),
                              Original.Imports.end()));
    // The loader resolved nothing itself; every cell was bound at load.
    for (const auto &I : Result.Imports)
      EXPECT_EQ(I.Origin, ImportOrigin::Static) << I.Name;
    const auto Path = Scratch / RebuiltFile;
    test::writeFile(Path, Result.Image);
    // The dispatch reads the pack record even when its program bytes are
    // unchanged. Compare the same mode in both independently loaded files.
    const auto Expected = runOriginal(Mode), Actual = run(Path);
    ASSERT_FALSE(HasFailure());
    EXPECT_EQ(Expected.Stop, ProcessStopReason::Exited) << Expected.Diagnostic;
    EXPECT_EQ(Expected.ExitStatus, ExitStatus);
    EXPECT_EQ(Expected.StandardOutput, Message);
    EXPECT_EQ(Actual.Stop, Expected.Stop) << Actual.Diagnostic;
    EXPECT_EQ(Actual.ExitStatus, Expected.ExitStatus);
    EXPECT_EQ(Actual.StandardOutput, Expected.StandardOutput);
    // The same program executes the same instructions.
    EXPECT_EQ(Actual.Instructions, Expected.Instructions);
  }

  /// Independently assembled code creates an entry inside its own page. The
  /// linked program remains unchanged and supplies the execution oracle.
  std::filesystem::path packSamePage(SamePageCode Kind) {
    using namespace llvm::support::endian;
    const auto *Relay = Original.section(RelaySection);
    if (!Relay || Relay->FileSize < 320) {
      ADD_FAILURE() << "the independent relay has no space for the loader";
      return {};
    }
    auto Bytes = Original.File;
    const uint32_t Target = Relay->RVA + 128;
    uint8_t *Code = Bytes.data() + Relay->FileOffset;
    std::fill_n(Code, Relay->FileSize, 0);
    const auto Index = Relay - Original.Sections.data();
    write32le(Bytes.data() + Original.SectionTableOffset + Index * 40 + 8,
              std::max(Relay->VirtualSize, Relay->FileSize));
    write32le(Bytes.data() + Original.EntryOffset, Relay->RVA);
    const bool MixedLoop = Kind == SamePageCode::MixedInstructionLoop;
    const bool Rewrite = Kind == SamePageCode::RewrittenCallback || MixedLoop;
    const bool Data = Kind == SamePageCode::AdjacentData;
    const bool Service = Kind == SamePageCode::ServiceWrite;
    uint64_t ModuleSlot = 0, LookupSlot = 0;
    if (Service) {
      for (const auto &I : Original.Imports) {
        if (I.Name == "GetModuleHandleA")
          ModuleSlot = I.Slot;
        if (I.Name == "GetProcAddress")
          LookupSlot = I.Slot;
      }
      if (!ModuleSlot || !LookupSlot) {
        ADD_FAILURE() << "the independent program has no export lookup imports";
        return {};
      }
      constexpr char Module[] = "kernel32.dll", Name[] = "WriteProcessMemory";
      std::copy(std::begin(Module), std::end(Module), Code + 160);
      std::copy(std::begin(Name), std::end(Name), Code + 176);
    }
    if (GetParam().ISA == GuestArchitecture::X64) {
      std::vector<uint8_t> Stub;
      auto Store = [&](uint32_t RVA, uint32_t Value, bool Byte) {
        const size_t At = Stub.size();
        Stub.insert(Stub.end(),
                    {uint8_t(Byte ? 0xc6 : 0xc7), 0x05, 0, 0, 0, 0});
        Stub.resize(Stub.size() + (Byte ? 1 : 4));
        write32le(Stub.data() + At + 2, RVA - (Relay->RVA + Stub.size()));
        if (Byte)
          Stub[At + 6] = Value;
        else
          write32le(Stub.data() + At + 6, Value);
      };
      auto Branch = [&](uint8_t Opcode, uint32_t RVA) {
        const size_t At = Stub.size();
        Stub.insert(Stub.end(), {Opcode, 0, 0, 0, 0});
        write32le(Stub.data() + At + 1, RVA - (Relay->RVA + Stub.size()));
      };
      if (Service) {
        auto Address = [&](uint8_t REX, uint8_t ModRM, uint32_t RVA) {
          const size_t At = Stub.size();
          Stub.insert(Stub.end(), {REX, 0x8d, ModRM, 0, 0, 0, 0});
          write32le(Stub.data() + At + 3, RVA - (Relay->RVA + Stub.size()));
        };
        auto Import = [&](uint32_t Slot) {
          const size_t At = Stub.size();
          Stub.insert(Stub.end(), {0xff, 0x15, 0, 0, 0, 0});
          write32le(Stub.data() + At + 2, Slot - (Relay->RVA + Stub.size()));
        };
        Stub.insert(Stub.end(), {0x48, 0x83, 0xec, 0x38}); // sub rsp, 56
        Address(0x48, 0x0d, Relay->RVA + 160);             // rcx = module name
        Import(ModuleSlot);
        Stub.insert(Stub.end(), {0x48, 0x89, 0xc1}); // mov rcx, rax
        Address(0x48, 0x15, Relay->RVA + 176);       // rdx = export name
        Import(LookupSlot);
        Stub.insert(Stub.end(), {0x48, 0xc7, 0xc1, 0xff, 0xff, 0xff, 0xff});
        Address(0x48, 0x15, Target);                       // rdx = destination
        Address(0x4c, 0x05, Relay->RVA + 136);             // r8 = source
        Stub.insert(Stub.end(), {0x41, 0xb9, 5, 0, 0, 0}); // mov r9d, 5
        Stub.insert(Stub.end(), {0x48, 0xc7, 0x44, 0x24, 0x20, 0, 0, 0, 0});
        Stub.insert(Stub.end(), {0xff, 0xd0});             // call rax
        Stub.insert(Stub.end(), {0x48, 0x83, 0xc4, 0x38}); // add rsp, 56
        Code[136] = 0xe9;
        write32le(Code + 137, Original.Entry - (Target + 5));
      } else if (Rewrite) {
        Stub.insert(Stub.end(), {0x48, 0x83, 0xec, 0x28}); // sub rsp, 40
        if (MixedLoop) {
          // Each complete loop instruction is generated, but its opcode
          // bytes remain from the loaded image. Only operands change.
          constexpr uint8_t Loop[] = {0xb9, 0, 0,    0, 0,   0x83,
                                      0xe9, 0, 0x75, 0, 0xc3};
          llvm::copy(Loop, Code + 128);
          Store(Target + 1, 1000000, false); // mov ecx, 1000000
          Store(Target + 7, 1, true);        // sub ecx, 1
          Store(Target + 9, 0xfb, true);     // jne to sub
        } else
          Store(Target, 0xc3, true); // ret
        Branch(0xe8, Target);
        Stub.insert(Stub.end(), {0x48, 0x83, 0xc4, 0x28}); // add rsp, 40
      }
      if (Data) {
        Code[128] = 0xe9;
        write32le(Code + 129, Original.Entry - (Target + 5));
        Store(Target + 5, 0xaa, true);
      } else if (!Service) {
        if (Kind == SamePageCode::ChangedOperand)
          Code[128] = 0xe9;
        Store(Target, 0xe9, true);
        Store(Target + 1, Original.Entry - (Target + 5), false);
      }
      Branch(0xe9, Target);
      llvm::copy(Stub, Code);
    } else {
      const uint32_t Jump =
          0x14000000 | ((Original.Entry - Target) >> 2 & 0x03ffffff);
      if (Service) {
        uint32_t At = 0, Literal = 256;
        auto Emit = [&](uint32_t Word) {
          write32le(Code + At, Word);
          At += 4;
        };
        auto Load = [&](unsigned Register, uint64_t Value) {
          Emit(0x58000000 | ((Literal - At) >> 2) << 5 | Register);
          write64le(Code + Literal, Value);
          Literal += 8;
        };
        auto Import = [&](uint32_t Slot) {
          Load(16, Original.Base + Slot);
          Emit(0xf9400210); // ldr x16, [x16]
          Emit(0xd63f0200); // blr x16
        };
        Emit(0xd100c3ff); // sub sp, sp, #48
        Emit(0xa9027bfd); // stp x29, x30, [sp, #32]
        Load(0, Original.Base + Relay->RVA + 160);
        Import(ModuleSlot);
        Load(1, Original.Base + Relay->RVA + 176);
        Import(LookupSlot);
        Emit(0xaa0003f0); // mov x16, x0
        Emit(0x92800000); // mov x0, #-1
        Load(1, Original.Base + Target);
        Load(2, Original.Base + Relay->RVA + 136);
        Emit(0xd2800083); // mov x3, #4
        Emit(0xaa1f03e4); // mov x4, xzr
        Emit(0xd63f0200); // blr x16
        Emit(0xa9427bfd); // ldp x29, x30, [sp, #32]
        Emit(0x9100c3ff); // add sp, sp, #48
        Load(16, Original.Base + Target);
        Emit(0xd61f0200); // br x16
        write32le(Code + 136, Jump);
      } else if (Rewrite) {
        write32le(Code, 0xa9bf7bfd);      // stp x29, x30, [sp, #-16]!
        write32le(Code + 4, 0x58000170);  // ldr x16, [pc, #44]
        write32le(Code + 8, 0x18000191);  // ldr w17, [pc, #48]
        write32le(Code + 12, 0xb9000211); // str w17, [x16]
        write32le(Code + 16, 0xd63f0200); // blr x16
        write32le(Code + 20, 0xa8c17bfd); // ldp x29, x30, [sp], #16
        write32le(Code + 24, 0x18000131); // ldr w17, [pc, #36]
        write32le(Code + 28, 0xb9000211); // str w17, [x16]
        write32le(Code + 32, 0xd61f0200); // br x16
        write64le(Code + 48, Original.Base + Target);
        write32le(Code + 56, 0xd65f03c0); // ret
        write32le(Code + 60, Jump);
      } else {
        write32le(Code, 0x58000090);     // ldr x16, [pc, #16]
        write32le(Code + 4, 0x180000b1); // ldr w17, [pc, #20]
        write32le(Code + 8, Data ? 0xb9000611 : 0xb9000211);
        write32le(Code + 12, 0xd61f0200); // br x16
        write64le(Code + 16, Original.Base + Target);
        write32le(Code + 24, Data ? 0xaabbccdd : Jump);
        if (Data || Kind == SamePageCode::ChangedOperand)
          write32le(Code + 128, Data ? Jump : 0x14000000);
      }
    }
    const auto Packed = Scratch / PackedFile;
    test::writeFile(Packed, Bytes);
    return Packed;
  }

  void expectSamePage(SamePageCode Kind) {
    const auto Packed = packSamePage(Kind);
    ASSERT_FALSE(HasFailure());
    const auto Expected = runOriginal(), PackedRun = run(Packed);
    ASSERT_FALSE(HasFailure());
    ASSERT_EQ(Expected.Stop, ProcessStopReason::Exited) << Expected.Diagnostic;
    ASSERT_EQ(Expected.ExitStatus, ExitStatus);
    ASSERT_EQ(Expected.StandardOutput, Message);
    ASSERT_EQ(PackedRun.Stop, Expected.Stop) << PackedRun.Diagnostic;
    ASSERT_EQ(PackedRun.ExitStatus, Expected.ExitStatus);
    ASSERT_EQ(PackedRun.StandardOutput, Expected.StandardOutput);
    auto Result = unpackFile(Packed, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    if (Kind == SamePageCode::AdjacentData) {
      EXPECT_EQ(Result->Outcome, UnpackOutcome::NoEntry) << Result->Diagnostic;
      EXPECT_TRUE(Result->Transfers.empty());
      EXPECT_EQ(Result->ProcessStop, test::StopExited);
      return;
    }
    ASSERT_EQ(Result->Outcome, UnpackOutcome::Unpacked) << Result->Diagnostic;
    const bool MixedLoop = Kind == SamePageCode::MixedInstructionLoop;
    const bool Rewrite = Kind == SamePageCode::RewrittenCallback || MixedLoop;
    ASSERT_EQ(Result->Transfers.size(), Rewrite ? 2u : 1u);
    const uint32_t Target = Original.section(RelaySection)->RVA + 128;
    EXPECT_EQ(Result->EntryRVA, Target);
    EXPECT_EQ(Result->Transfers.back().Generation, Rewrite ? 2u : 1u);
    EXPECT_TRUE(Result->Transfers.back().StackBalanced);
    EXPECT_TRUE(Result->Transfers.back().ProgramInvocation);
    if (Rewrite) {
      EXPECT_EQ(Result->Transfers.front().RVA, Target);
      EXPECT_EQ(Result->Transfers.front().Generation, 1u);
      EXPECT_FALSE(Result->Transfers.front().StackBalanced);
    }
    const auto Rebuilt = Scratch / RebuiltFile;
    test::writeFile(Rebuilt, Result->Image);
    const auto Actual = run(Rebuilt);
    ASSERT_FALSE(HasFailure());
    EXPECT_EQ(Actual.Stop, Expected.Stop) << Actual.Diagnostic;
    EXPECT_EQ(Actual.ExitStatus, Expected.ExitStatus);
    EXPECT_EQ(Actual.StandardOutput, Expected.StandardOutput);
  }

  Image Original;
  std::filesystem::path Scratch;
  UnpackOptions Options;
};

TEST_P(UnpackGenerated, PackedProgramRunsLikeTheLinkedOne) {
  // The oracle itself: packing changed where the code comes from, not what
  // the process does.
  const auto Expected = runOriginal();
  ASSERT_FALSE(HasFailure());
  for (const uint32_t Mode : {JumpMode, CallMode, StagedMode}) {
    SCOPED_TRACE(Mode);
    const auto Packed = pack(Mode);
    ASSERT_FALSE(HasFailure());
    const auto Actual = run(Packed);
    ASSERT_FALSE(HasFailure());
    EXPECT_EQ(Actual.Stop, ProcessStopReason::Exited) << Actual.Diagnostic;
    EXPECT_EQ(Actual.ExitStatus, Expected.ExitStatus);
    EXPECT_EQ(Actual.StandardOutput, Expected.StandardOutput);
    if (GetParam().Contract == ExecutionContract::DirectUserX64)
      EXPECT_EQ(Actual.Instructions, 0u);
    else
      EXPECT_GT(Actual.Instructions, Expected.Instructions);
  }
}

TEST_P(UnpackGenerated, LoaderThatLeavesForTheProgramYieldsTheLinkedImage) {
  const auto Result = unpack(JumpMode);
  ASSERT_FALSE(HasFailure());
  ASSERT_EQ(Result.Outcome, UnpackOutcome::Unpacked) << Result.Diagnostic;
  // No protector is involved, so none is named and none declares an entry.
  EXPECT_EQ(Result.Packer.Kind, PackerKind::Unidentified);
  EXPECT_TRUE(Result.Packer.Evidence.empty());
  EXPECT_EQ(Result.Format, FormatKind::PE64);
  EXPECT_EQ(Result.Architecture, guestArchitectureName(GetParam().ISA));
  EXPECT_EQ(Result.Profile, processProfileName(ProcessProfile::WindowsPE64));
  ASSERT_EQ(Result.Transfers.size(), 1u);
  EXPECT_EQ(Result.Transfers[0].RVA, Original.Entry);
  EXPECT_TRUE(Result.Transfers[0].StackBalanced);
  EXPECT_EQ(Result.Transfers[0].Generation, 1u);
  EXPECT_EQ(Result.EntryRVA, Original.Entry);
  EXPECT_EQ(Result.Source, EntrySource::Transfer);
  expectOriginalProgram(Result);
}

TEST_P(UnpackGenerated,
       LiveHeapReferencesCannotPublishAnOrdinaryRecoveredImage) {
  for (bool TLSOnly : {false, true}) {
    SCOPED_TRACE(TLSOnly);
    if (TLSOnly)
      useTLSHeapFixture();
    ASSERT_FALSE(HasFailure());
    const auto Packed = pack(HeapStateMode);
    ASSERT_FALSE(HasFailure());
    const auto OriginalRun = run(Packed);
    ASSERT_EQ(OriginalRun.Stop, ProcessStopReason::Exited)
        << OriginalRun.Diagnostic;
    EXPECT_EQ(OriginalRun.ExitStatus, ExitStatus);
    EXPECT_EQ(OriginalRun.StandardOutput, Message);
    const auto Result = unpack(HeapStateMode);
    ASSERT_FALSE(HasFailure());
    EXPECT_EQ(Result.Outcome, UnpackOutcome::UnsupportedState);
    EXPECT_EQ(Result.EntryRVA, Original.Entry);
    EXPECT_TRUE(Result.Image.empty());
    EXPECT_TRUE(Result.RuntimeState.HeapInventoryKnown);
    EXPECT_GT(Result.RuntimeState.PossibleHeapReferences, 0u);
    EXPECT_FALSE(Result.RuntimeState.HeapReferences.empty());
    EXPECT_NE(Result.Diagnostic.find("heap allocations"), std::string::npos);
    if (TLSOnly) {
      unsigned TLSReferences = 0;
      for (const auto &Reference : Result.RuntimeState.HeapReferences)
        if (Reference.Location == UnpackHeapReference::Storage::ThreadLocal) {
          ++TLSReferences;
          EXPECT_EQ(Reference.Offset, 3u);
        }
      EXPECT_EQ(TLSReferences, 1u);
    }
  }
}

TEST_P(UnpackGenerated, ExplicitSnapshotsKeepExternalHeapDependenciesVisible) {
  for (bool TLSOnly : {false, true}) {
    SCOPED_TRACE(TLSOnly);
    if (TLSOnly)
      useTLSHeapFixture();
    ASSERT_FALSE(HasFailure());
    Options.SnapshotOnly = true;
    const auto Result = unpack(HeapStateMode);
    ASSERT_FALSE(HasFailure());
    ASSERT_EQ(Result.Outcome, UnpackOutcome::Snapshot) << Result.Diagnostic;
    ASSERT_FALSE(Result.Image.empty());
    EXPECT_GT(Result.RuntimeState.PossibleHeapReferences, 0u);
    const auto Path = Scratch / RebuiltFile;
    test::writeFile(Path, Result.Image);
    const auto Replay = run(Path);
    EXPECT_EQ(Replay.Stop, ProcessStopReason::CPUFailure) << Replay.Diagnostic;
    EXPECT_FALSE(Replay.ExitStatus);
    EXPECT_TRUE(Replay.StandardOutput.empty());
  }
}

TEST_P(UnpackGenerated,
       MaterializedRuntimePreservesOwnedObjectsOnNativeWindows) {
  if (GetParam().ISA != GuestArchitecture::X64)
    GTEST_SKIP() << "the native materializer currently emits x64 PE code";
  ASSERT_TRUE(bool(llvm::sys::findProgramByName("clang")));
  ASSERT_TRUE(bool(llvm::sys::findProgramByName("lld-link")));
  WindowsPEBVersion Version{10, 0, 19043, 2};
#if defined(_WIN32) && defined(_M_X64)
  // Independent native environment input; WindowsSystemNative separately
  // compares these bytes with RtlGetVersion on this same test host.
  const auto *PEB = reinterpret_cast<const uint8_t *>(__readgsqword(0x60));
  Version = {llvm::support::endian::read32le(PEB + 0x118),
             llvm::support::endian::read32le(PEB + 0x11c),
             llvm::support::endian::read16le(PEB + 0x120),
             llvm::support::endian::read32le(PEB + 0x124)};
#endif
  Options.Process.Windows->PEBVersion = Version;
  for (unsigned Mode :
       {HeapStateMode, EncodedPointerMode, NativeEncodedPointerMode,
        DynamicFLSMode, ZeroDynamicFLSMode, OwnedRuntimeMode,
        VirtualRuntimeMode}) {
    SCOPED_TRACE(Mode);
    const auto Packed = pack(Mode);
    ASSERT_FALSE(HasFailure());
    const auto OriginalRun = run(Packed);
    ASSERT_EQ(OriginalRun.Stop, ProcessStopReason::Exited)
        << OriginalRun.Diagnostic;
    ASSERT_EQ(OriginalRun.ExitStatus, ExitStatus);
    expectNativeWindows(Packed, ExitStatus);
    ASSERT_FALSE(HasFailure());
    Options.RestoreRuntime = true;
    const auto Restored = unpack(Mode);
    ASSERT_FALSE(HasFailure());
    ASSERT_EQ(Restored.Outcome, UnpackOutcome::Restored) << Restored.Diagnostic;
    ASSERT_FALSE(Restored.Image.empty());
    EXPECT_EQ(Restored.ImportRepair.ObservedCalls, 0u);
    const auto Output = Scratch / RebuiltFile;
    test::writeFile(Output, Restored.Image);
    const auto Image = test::readImage(Restored.Image);
    ASSERT_FALSE(HasFailure());
    // Windows requires adjacent section RVAs. Wine accepts holes, so check
    // this loader contract even on hosts that cannot launch a native PE.
    llvm::object::pe32plus_header PE;
    std::memcpy(
        &PE,
        Image.File.data() + Image.EntryOffset -
            offsetof(llvm::object::pe32plus_header, AddressOfEntryPoint),
        sizeof(PE));
    uint64_t Next = llvm::alignTo(uint64_t(PE.SizeOfHeaders),
                                  uint64_t(PE.SectionAlignment));
    for (const auto &Section : Image.Sections) {
      EXPECT_EQ(Section.RVA, Next) << Section.Name;
      Next = llvm::alignTo(uint64_t(Section.RVA) +
                               std::max(Section.VirtualSize, Section.FileSize),
                           uint64_t(PE.SectionAlignment));
    }
    EXPECT_EQ(Next, PE.SizeOfImage);
    // The native loader must be able to bind every import before TLS runs.
    // Read-only cells need the PE's IAT protection range, even when a second
    // import provider table is appended by the runtime materializer.
    const auto IAT = Image.directory(llvm::COFF::IAT);
    const uint64_t IATBegin = IAT.RelativeVirtualAddress;
    const uint64_t IATEnd = IATBegin + IAT.Size;
    for (const auto &Import : Image.Imports) {
      const auto Section = llvm::find_if(Image.Sections, [&](const auto &S) {
        return Import.Slot >= S.RVA &&
               Import.Slot + 8 <= uint64_t(S.RVA) + S.VirtualSize;
      });
      ASSERT_NE(Section, Image.Sections.end());
      if (!(Section->Characteristics & llvm::COFF::IMAGE_SCN_MEM_WRITE)) {
        EXPECT_GE(Import.Slot, IATBegin) << Import.Name;
        EXPECT_LE(Import.Slot + 8, IATEnd) << Import.Name;
      }
    }
    expectNativeWindows(Output, ExitStatus);
    ASSERT_FALSE(HasFailure());
    for (const auto &Section : Image.Sections)
      if (llvm::StringRef(Section.Name).starts_with(".nd"))
        EXPECT_FALSE(
            (Section.Characteristics & llvm::COFF::IMAGE_SCN_MEM_WRITE) &&
            (Section.Characteristics & llvm::COFF::IMAGE_SCN_MEM_EXECUTE));
  }
}

TEST_P(UnpackGenerated, MaterializationRequiresKnownSupportedState) {
  for (unsigned Mode : {OwnedRuntimeMode, VirtualRuntimeMode}) {
    SCOPED_TRACE(Mode);
    const auto Default = unpack(Mode);
    ASSERT_FALSE(HasFailure());
    EXPECT_EQ(Default.Outcome, UnpackOutcome::UnsupportedState);
    EXPECT_TRUE(Default.Image.empty());
    EXPECT_TRUE(Default.RuntimeState.AdditionalStateInventoryKnown);
    EXPECT_TRUE(Default.RuntimeState.HasAdditionalDependencies);
    Options.SnapshotOnly = true;
    const auto Snapshot = unpack(Mode);
    ASSERT_FALSE(HasFailure());
    EXPECT_EQ(Snapshot.Outcome, UnpackOutcome::Snapshot);
    EXPECT_TRUE(Snapshot.RuntimeState.HasAdditionalDependencies);
    EXPECT_FALSE(Snapshot.Image.empty());
    Options.SnapshotOnly = false;
  }
  Options.RestoreRuntime = true;
  const auto Missing = unpack(HeapStateMode);
  ASSERT_FALSE(HasFailure());
  EXPECT_EQ(Missing.Outcome, UnpackOutcome::UnsupportedState);
  EXPECT_TRUE(Missing.Image.empty());
  Options.Process.Windows->PEBVersion = WindowsPEBVersion{10, 0, 19043, 2};
  for (unsigned Mode :
       {DynamicTLSMode, ZeroDynamicTLSMode, UnallocatedTLSValueMode}) {
    SCOPED_TRACE(Mode);
    const auto Refused = unpack(Mode);
    ASSERT_FALSE(HasFailure());
    EXPECT_EQ(Refused.Outcome, UnpackOutcome::UnsupportedState);
    EXPECT_FALSE(Refused.Diagnostic.empty());
    EXPECT_TRUE(Refused.Image.empty());
  }
}

TEST_P(UnpackGenerated, DirectServiceBindingsRequireAnExplicitSnapshot) {
  if (GetParam().ISA != GuestArchitecture::X64)
    GTEST_SKIP() << "the direct service fixture uses the x64 Windows ABI";
  for (unsigned Mode : {DirectServiceMode, LateDirectServiceMode}) {
    SCOPED_TRACE(Mode);
    auto Packed = pack(Mode);
    ASSERT_FALSE(HasFailure());
    auto OriginalRun = run(Packed);
    ASSERT_EQ(OriginalRun.Stop, ProcessStopReason::Exited)
        << OriginalRun.Diagnostic;
    EXPECT_EQ(OriginalRun.ExitStatus, ExitStatus);
    EXPECT_EQ(OriginalRun.StandardOutput, Message);
    for (bool Snapshot : {false, true}) {
      SCOPED_TRACE(Snapshot);
      Options.SnapshotOnly = Snapshot;
      auto Result = unpack(Mode);
      ASSERT_FALSE(HasFailure());
      EXPECT_EQ(Result.Outcome, Snapshot ? UnpackOutcome::Snapshot
                                         : UnpackOutcome::UnsupportedState);
      EXPECT_EQ(Result.Image.empty(), !Snapshot);
      EXPECT_EQ(Result.RuntimeState.PossibleHeapReferences, 0u);
      EXPECT_GT(Result.RuntimeState.DirectServiceCalls, 0u);
      EXPECT_NE(Result.Diagnostic.find("native binding"), std::string::npos);
    }
    // Not selecting a transfer does not erase calls that actually executed.
    Options.Transfer = defaults::MaxTransfers;
    const auto MissingEntry = unpack(Mode);
    ASSERT_FALSE(HasFailure());
    EXPECT_EQ(MissingEntry.Outcome, UnpackOutcome::NoEntry);
    EXPECT_EQ(MissingEntry.ProcessStop,
              processStopReasonName(ProcessStopReason::Exited));
    EXPECT_FALSE(MissingEntry.EntryRVA);
    EXPECT_TRUE(MissingEntry.Image.empty());
    EXPECT_FALSE(MissingEntry.RuntimeState.HeapInventoryKnown);
    EXPECT_EQ(MissingEntry.RuntimeState.DirectServiceCalls,
              llvm::count_if(OriginalRun.NativeCalls, [](const auto &Call) {
                return Call.DirectServiceNumber.has_value();
              }));
    EXPECT_GT(MissingEntry.RuntimeState.DirectServiceCalls, 0u);
    Options.Transfer = 0;
  }
}

TEST_P(UnpackGenerated, ReleasedHeapStateDoesNotBlockRecovery) {
  for (bool TLSOnly : {false, true}) {
    SCOPED_TRACE(TLSOnly);
    if (TLSOnly)
      useTLSHeapFixture();
    ASSERT_FALSE(HasFailure());
    const auto Packed = pack(ReleasedHeapStateMode);
    ASSERT_FALSE(HasFailure());
    const auto Expected = run(Packed);
    ASSERT_EQ(Expected.Stop, ProcessStopReason::Exited) << Expected.Diagnostic;
    EXPECT_EQ(Expected.ExitStatus, ExitStatus);
    const auto Result = unpack(ReleasedHeapStateMode);
    ASSERT_FALSE(HasFailure());
    ASSERT_EQ(Result.Outcome, UnpackOutcome::Unpacked) << Result.Diagnostic;
    EXPECT_TRUE(Result.RuntimeState.HeapInventoryKnown);
    EXPECT_EQ(Result.RuntimeState.PossibleHeapReferences, 0u);
    const auto Path = Scratch / RebuiltFile;
    test::writeFile(Path, Result.Image);
    const auto Actual = run(Path);
    EXPECT_EQ(Actual.Stop, Expected.Stop) << Actual.Diagnostic;
    EXPECT_EQ(Actual.ExitStatus, Expected.ExitStatus);
    EXPECT_EQ(Actual.StandardOutput, Expected.StandardOutput);
  }
}

TEST_P(UnpackGenerated, CapturedEncodedPointersRequireAnExplicitSnapshot) {
  for (bool TLSOnly : {false, true}) {
    SCOPED_TRACE(TLSOnly);
    if (TLSOnly)
      useTLSHeapFixture();
    for (unsigned Mode : {EncodedPointerMode, NativeEncodedPointerMode,
                          DecodedPointerMode, NativeDecodedPointerMode}) {
      SCOPED_TRACE(Mode);
      const auto Packed = pack(Mode);
      ASSERT_FALSE(HasFailure());
      const auto Expected = run(Packed);
      ASSERT_EQ(Expected.Stop, ProcessStopReason::Exited)
          << Expected.Diagnostic;
      ASSERT_EQ(Expected.ExitStatus, ExitStatus);
      EXPECT_EQ(Expected.StandardOutput, Message);
      for (bool Snapshot : {false, true}) {
        SCOPED_TRACE(Snapshot);
        Options.SnapshotOnly = Snapshot;
        const auto Result = unpack(Mode);
        ASSERT_FALSE(HasFailure());
        EXPECT_EQ(Result.Outcome, Snapshot ? UnpackOutcome::Snapshot
                                           : UnpackOutcome::UnsupportedState);
        EXPECT_EQ(Result.Image.empty(), !Snapshot);
        EXPECT_EQ(Result.RuntimeState.PossibleHeapReferences, 0u);
        EXPECT_EQ(Result.RuntimeState.DirectServiceCalls, 0u);
        EXPECT_TRUE(Result.RuntimeState.EncodedPointerInventoryKnown);
        EXPECT_EQ(Result.RuntimeState.PossibleEncodedPointers, 1u);
        ASSERT_EQ(Result.RuntimeState.EncodedPointerReferences.size(), 1u);
        EXPECT_EQ(Result.RuntimeState.EncodedPointerReferences[0].Location,
                  TLSOnly ? UnpackHeapReference::Storage::ThreadLocal
                          : UnpackHeapReference::Storage::Image);
        EXPECT_NE(Result.Diagnostic.find("encoded pointer"), std::string::npos);
      }
    }
  }
}

TEST_P(UnpackGenerated, ClearedAndPostEntryEncodingsDoNotBlockRecovery) {
  for (bool TLSOnly : {false, true}) {
    SCOPED_TRACE(TLSOnly);
    if (TLSOnly)
      useTLSHeapFixture();
    for (unsigned Mode : {ClearedEncodedPointerMode, LateEncodedPointerMode}) {
      SCOPED_TRACE(Mode);
      const auto Result = unpack(Mode);
      ASSERT_FALSE(HasFailure());
      ASSERT_EQ(Result.Outcome, UnpackOutcome::Unpacked) << Result.Diagnostic;
      EXPECT_TRUE(Result.RuntimeState.EncodedPointerInventoryKnown);
      EXPECT_EQ(Result.RuntimeState.PossibleEncodedPointers, 0u);
      const auto Path = Scratch / RebuiltFile;
      test::writeFile(Path, Result.Image);
      const auto Actual = run(Path);
      EXPECT_EQ(Actual.Stop, ProcessStopReason::Exited) << Actual.Diagnostic;
      EXPECT_EQ(Actual.ExitStatus, ExitStatus);
      EXPECT_EQ(Actual.StandardOutput, Message);
    }
  }
}

TEST_P(UnpackGenerated, LiveDynamicSlotsRequireAnExplicitSnapshot) {
  for (unsigned Mode : {DynamicTLSMode, DynamicFLSMode, ZeroDynamicTLSMode,
                        ZeroDynamicFLSMode, UnallocatedTLSValueMode}) {
    SCOPED_TRACE(Mode);
    const auto Packed = pack(Mode);
    ASSERT_FALSE(HasFailure());
    const auto Expected = run(Packed);
    ASSERT_EQ(Expected.Stop, ProcessStopReason::Exited) << Expected.Diagnostic;
    ASSERT_EQ(Expected.ExitStatus, ExitStatus);
    EXPECT_EQ(Expected.StandardOutput, Message);
    expectNativeWindows(Packed, ExitStatus);
    for (bool Snapshot : {false, true}) {
      SCOPED_TRACE(Snapshot);
      Options.SnapshotOnly = Snapshot;
      const auto Result = unpack(Mode);
      ASSERT_FALSE(HasFailure());
      EXPECT_EQ(Result.Outcome, Snapshot ? UnpackOutcome::Snapshot
                                         : UnpackOutcome::UnsupportedState);
      EXPECT_EQ(Result.Image.empty(), !Snapshot);
      EXPECT_EQ(Result.RuntimeState.PossibleHeapReferences, 0u);
      EXPECT_EQ(Result.RuntimeState.PossibleEncodedPointers, 0u);
      EXPECT_EQ(Result.RuntimeState.DirectServiceCalls, 0u);
      EXPECT_TRUE(Result.RuntimeState.DynamicThreadLocalInventoryKnown);
      const bool Fiber = Mode == DynamicFLSMode || Mode == ZeroDynamicFLSMode;
      EXPECT_EQ(Result.RuntimeState.LiveDynamicTLSSlots, Fiber ? 0u : 1u);
      EXPECT_EQ(Result.RuntimeState.LiveDynamicFLSSlots, Fiber ? 1u : 0u);
      EXPECT_NE(Result.Diagnostic.find("dynamic TLS/FLS"), std::string::npos);
      if (Snapshot && (Mode == DynamicTLSMode || Mode == DynamicFLSMode ||
                       Mode == UnallocatedTLSValueMode)) {
        const auto Path = Scratch / RebuiltFile;
        test::writeFile(Path, Result.Image);
        const auto Replay = run(Path);
        EXPECT_EQ(Replay.Stop, ProcessStopReason::Exited) << Replay.Diagnostic;
        EXPECT_EQ(Replay.ExitStatus, FailureStatus);
        EXPECT_EQ(Replay.StandardOutput, Message);
        expectNativeWindows(Path, FailureStatus);
      }
    }
  }
}

TEST_P(UnpackGenerated, ReleasedAndPostEntryDynamicSlotsDoNotBlockRecovery) {
  for (unsigned Mode :
       {ReleasedDynamicTLSMode, ReleasedDynamicFLSMode, LateDynamicTLSMode,
        LateDynamicFLSMode, ClearedUnallocatedTLSValueMode}) {
    SCOPED_TRACE(Mode);
    const auto Packed = pack(Mode);
    ASSERT_FALSE(HasFailure());
    expectNativeWindows(Packed, ExitStatus);
    const auto Result = unpack(Mode);
    ASSERT_FALSE(HasFailure());
    ASSERT_EQ(Result.Outcome, UnpackOutcome::Unpacked) << Result.Diagnostic;
    EXPECT_TRUE(Result.RuntimeState.DynamicThreadLocalInventoryKnown);
    EXPECT_EQ(Result.RuntimeState.LiveDynamicTLSSlots, 0u);
    EXPECT_EQ(Result.RuntimeState.LiveDynamicFLSSlots, 0u);
    const auto Path = Scratch / RebuiltFile;
    test::writeFile(Path, Result.Image);
    const auto Actual = run(Path);
    EXPECT_EQ(Actual.Stop, ProcessStopReason::Exited) << Actual.Diagnostic;
    EXPECT_EQ(Actual.ExitStatus, ExitStatus);
    EXPECT_EQ(Actual.StandardOutput, Message);
    expectNativeWindows(Path, ExitStatus);
  }
}

TEST_P(UnpackGenerated, FLSCleanupCallbacksCannotBeSilentlyDiscarded) {
  for (unsigned Mode :
       {FreedFLSCallbackMode, NestedFLSCallbackMode, RearmedFLSCallbackMode}) {
    SCOPED_TRACE(Mode);
    const auto Packed = pack(Mode);
    ASSERT_FALSE(HasFailure());
    expectNativeWindows(Packed, ExitStatus);
    const auto Executed = run(Packed);
    ASSERT_EQ(Executed.Stop, ProcessStopReason::Exited) << Executed.Diagnostic;
    EXPECT_EQ(Executed.ExitStatus, ExitStatus);
    EXPECT_EQ(Executed.StandardOutput, Message);
    for (const auto &Call : Executed.NativeCalls)
      if (Call.Name == "FlsFree")
        EXPECT_EQ(Call.Result, 1u);
    if (Mode == NestedFLSCallbackMode || Mode == RearmedFLSCallbackMode) {
      struct Invocations final : ProcessObserver {
        std::vector<bool> Program;
        llvm::Expected<std::vector<ExecutionWatch>>
        started(ProcessView &P) override {
          Program.push_back(P.programInvocation());
          return std::vector<ExecutionWatch>{};
        }
        llvm::Expected<std::optional<std::vector<ExecutionWatch>>>
        invoking(ProcessView &P) override {
          Program.push_back(P.programInvocation());
          EXPECT_TRUE(P.completedInitializers().empty());
          return std::nullopt;
        }
        llvm::Expected<std::optional<std::vector<ExecutionWatch>>>
        watched(ProcessView &, uint64_t) override {
          ADD_FAILURE() << "unexpected observer watch";
          return std::nullopt;
        }
      } Observer;
      auto Observed = observeProcess(Packed, ProcessProfile::WindowsPE64,
                                     Options.Process, Observer);
      ASSERT_TRUE(bool(Observed)) << llvm::toString(Observed.takeError());
      EXPECT_EQ(Observed->ExitStatus, ExitStatus);
      std::vector<bool> Expected(
          Mode == NestedFLSCallbackMode ? 5 : FLSRearmCount + 2, false);
      Expected.front() = Expected.back() = true;
      EXPECT_EQ(Observer.Program, Expected);
    }
    for (bool Snapshot : {false, true}) {
      SCOPED_TRACE(Snapshot);
      Options.SnapshotOnly = Snapshot;
      const auto Result = unpack(Mode);
      ASSERT_FALSE(HasFailure());
      ASSERT_EQ(Result.Outcome,
                Snapshot ? UnpackOutcome::Snapshot : UnpackOutcome::Unpacked)
          << Result.Diagnostic;
      EXPECT_TRUE(Result.RuntimeState.DynamicThreadLocalInventoryKnown);
      EXPECT_EQ(Result.RuntimeState.LiveDynamicFLSSlots, 0u);
      EXPECT_EQ(Result.RuntimeState.PossibleHeapReferences, 0u);
      const auto Path = Scratch / RebuiltFile;
      test::writeFile(Path, Result.Image);
      const auto Actual = run(Path);
      EXPECT_EQ(Actual.Stop, ProcessStopReason::Exited) << Actual.Diagnostic;
      EXPECT_EQ(Actual.ExitStatus, ExitStatus);
      EXPECT_EQ(Actual.StandardOutput, Message);
      expectNativeWindows(Path, ExitStatus);
    }
  }
}

TEST_P(UnpackGenerated, RearmedFLSCleanupUsesTheProcessEventBudget) {
  const auto Packed = pack(EndlessFLSCallbackMode);
  ASSERT_FALSE(HasFailure());
  Options.Process.Limits.Events = 600;
  const auto Executed = run(Packed);
  EXPECT_EQ(Executed.Stop, ProcessStopReason::EventLimit)
      << Executed.Diagnostic;
  EXPECT_EQ(Executed.Events, Options.Process.Limits.Events);
  EXPECT_FALSE(Executed.ExitStatus);
  EXPECT_TRUE(Executed.StandardOutput.empty());
  EXPECT_GT(llvm::count_if(
                Executed.NativeCalls,
                [](const auto &Call) { return Call.Name == "FlsSetValue"; }),
            FLSRearmCount);
  for (const auto &Call : Executed.NativeCalls)
    if (Call.Name == "FlsFree")
      EXPECT_FALSE(Call.Result);
}

TEST_P(UnpackGenerated, FLSCleanupRejectsRecursiveFreeOfTheActiveSlot) {
  const auto Packed = pack(RecursiveFLSCallbackMode);
  ASSERT_FALSE(HasFailure());
  const auto Executed = run(Packed);
  EXPECT_EQ(Executed.Stop, ProcessStopReason::UnsupportedService)
      << Executed.Diagnostic;
  EXPECT_EQ(
      llvm::count_if(Executed.NativeCalls,
                     [](const auto &Call) { return Call.Name == "FlsFree"; }),
      2);
  const auto Result = unpack(RecursiveFLSCallbackMode);
  ASSERT_FALSE(HasFailure());
  EXPECT_EQ(Result.Outcome, UnpackOutcome::NoEntry);
  EXPECT_TRUE(Result.Image.empty());
  EXPECT_EQ(Result.ProcessStop, "unsupported_service");
}

TEST_P(UnpackGenerated, EmptyAndReusedFLSSlotsDoNotInvokeStaleCallbacks) {
  for (unsigned Mode : {EmptyFLSCallbackMode, ReusedFLSCallbackMode}) {
    SCOPED_TRACE(Mode);
    const auto Packed = pack(Mode);
    ASSERT_FALSE(HasFailure());
    expectNativeWindows(Packed, ExitStatus);
    const auto Result = unpack(Mode);
    ASSERT_FALSE(HasFailure());
    ASSERT_EQ(Result.Outcome, UnpackOutcome::Unpacked) << Result.Diagnostic;
    EXPECT_TRUE(Result.RuntimeState.DynamicThreadLocalInventoryKnown);
    EXPECT_EQ(Result.RuntimeState.LiveDynamicFLSSlots, 0u);
    const auto Path = Scratch / RebuiltFile;
    test::writeFile(Path, Result.Image);
    const auto Actual = run(Path);
    EXPECT_EQ(Actual.Stop, ProcessStopReason::Exited) << Actual.Diagnostic;
    EXPECT_EQ(Actual.ExitStatus, ExitStatus);
    EXPECT_EQ(Actual.StandardOutput, Message);
    expectNativeWindows(Path, ExitStatus);
  }
}

TEST_P(UnpackGenerated, LoaderGeneratesItsEntryInThePageItIsExecuting) {
  expectSamePage(SamePageCode::GeneratedJump);
}

TEST_P(UnpackGenerated, LoaderRewritesACallbackInItsOwnPageBeforeEnteringIt) {
  expectSamePage(SamePageCode::RewrittenCallback);
}

TEST_P(UnpackGenerated,
       MixedGenerationInstructionsDoNotRetriggerEveryIteration) {
  if (GetParam().Contract != ExecutionContract::DirectUserX64)
    GTEST_SKIP() << "the long native loop requires direct x64 execution";
  Options.Process.Limits.TimeoutMicroseconds = 10000000;
  expectSamePage(SamePageCode::MixedInstructionLoop);
}

TEST_P(UnpackGenerated, ChangedDataBesideAnUnchangedInstructionIsNotAnEntry) {
  expectSamePage(SamePageCode::AdjacentData);
}

TEST_P(UnpackGenerated, ChangedOperandsInTheExecutingPageEstablishANewEntry) {
  expectSamePage(SamePageCode::ChangedOperand);
}

TEST_P(UnpackGenerated, StoppedServiceWritesCanGenerateCodeInAnExecutedPage) {
  expectSamePage(SamePageCode::ServiceWrite);
}

TEST_P(UnpackGenerated, GeneratedOperandAcrossAPageBoundaryEstablishesAnEntry) {
  if (GetParam().ISA != GuestArchitecture::X64)
    GTEST_SKIP() << "aligned ARM64 instructions cannot straddle a 4 KiB page";
  using namespace llvm::support::endian;
  const auto *Record = Original.section(PackSection);
  ASSERT_NE(Record, nullptr);
  ASSERT_GE(Record->FileSize, 8200u);
  auto Bytes = Original.File;
  const uint32_t Entry = Record->RVA + 4096, Target = Record->RVA + 8190;
  const uint32_t Relative = Original.Entry - (Target + 5);
  uint8_t *Code = Bytes.data() + Record->FileOffset + 4096;
  const uint8_t Stub[] = {0xc7, 0x05, 0, 0, 0, 0, 0, 0, 0, 0, 0xe9, 0, 0, 0, 0};
  llvm::copy(Stub, Code);
  write32le(Code + 2, Target + 2 - (Entry + 10));
  write32le(Code + 6, Relative >> 8);
  write32le(Code + 11, Target - (Entry + sizeof Stub));
  // The opcode and first operand byte belong to an executed page and never
  // change. Only bytes in its unvisited successor page are generated.
  Code[4094] = 0xe9;
  Code[4095] = Relative;
  const auto Index = Record - Original.Sections.data();
  write32le(Bytes.data() + Original.SectionTableOffset + Index * 40 + 36,
            Record->Characteristics | llvm::COFF::IMAGE_SCN_MEM_EXECUTE);
  write32le(Bytes.data() + Original.EntryOffset, Entry);
  const auto Packed = Scratch / PackedFile;
  test::writeFile(Packed, Bytes);
  const auto Expected = runOriginal(), PackedRun = run(Packed);
  ASSERT_FALSE(HasFailure());
  ASSERT_EQ(PackedRun.Stop, ProcessStopReason::Exited) << PackedRun.Diagnostic;
  ASSERT_EQ(PackedRun.ExitStatus, Expected.ExitStatus);
  ASSERT_EQ(PackedRun.StandardOutput, Expected.StandardOutput);
  auto Result = unpackFile(Packed, Options);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Outcome, UnpackOutcome::Unpacked) << Result->Diagnostic;
  ASSERT_EQ(Result->Transfers.size(), 1u);
  EXPECT_EQ(Result->EntryRVA, Target);
  EXPECT_EQ(Result->Transfers.front().Generation, 1u);
  const auto Rebuilt = Scratch / RebuiltFile;
  test::writeFile(Rebuilt, Result->Image);
  const auto Actual = run(Rebuilt);
  ASSERT_FALSE(HasFailure());
  EXPECT_EQ(Actual.Stop, Expected.Stop) << Actual.Diagnostic;
  EXPECT_EQ(Actual.ExitStatus, Expected.ExitStatus);
  EXPECT_EQ(Actual.StandardOutput, Expected.StandardOutput);
}

TEST_P(UnpackGenerated, LoaderCallIntoTheProgramPrecedesItsEntry) {
  const auto Result = unpack(CallMode);
  ASSERT_FALSE(HasFailure());
  ASSERT_EQ(Result.Outcome, UnpackOutcome::Unpacked) << Result.Diagnostic;
  // The call enters generated code on a deeper stack and returns. The jump
  // that follows enters the same generation on the entry stack.
  ASSERT_EQ(Result.Transfers.size(), 2u);
  EXPECT_FALSE(Result.Transfers[0].StackBalanced);
  EXPECT_NE(Result.Transfers[0].RVA, Original.Entry);
  EXPECT_EQ(Result.Transfers[0].Generation, 1u);
  EXPECT_TRUE(Result.Transfers[1].StackBalanced);
  EXPECT_EQ(Result.Transfers[1].RVA, Original.Entry);
  EXPECT_EQ(Result.Transfers[1].Generation, 1u);
  EXPECT_EQ(Result.EntryRVA, Original.Entry);
  EXPECT_EQ(Result.Source, EntrySource::Transfer);
  expectOriginalProgram(Result);
}

TEST_P(UnpackGenerated, SecondLoaderIsTheEntryUntilALaterTransferIsNamed) {
  const auto *Relay = Original.section(RelaySection);
  ASSERT_NE(Relay, nullptr);
  // The first loader leaves on the entry stack for code it wrote. Nothing in
  // the image says that this code is another loader.
  const auto First = unpack(StagedMode);
  ASSERT_FALSE(HasFailure());
  ASSERT_EQ(First.Outcome, UnpackOutcome::Unpacked) << First.Diagnostic;
  ASSERT_EQ(First.Transfers.size(), 1u);
  EXPECT_EQ(First.EntryRVA, Relay->RVA);
  EXPECT_EQ(First.Transfers[0].Generation, 1u);
  // That image is still a program: its entry writes the rest and leaves.
  const auto Partial = Scratch / RebuiltFile;
  test::writeFile(Partial, First.Image);
  const auto Ran = run(Partial);
  ASSERT_FALSE(HasFailure());
  EXPECT_EQ(Ran.Stop, ProcessStopReason::Exited) << Ran.Diagnostic;
  EXPECT_EQ(Ran.ExitStatus, ExitStatus);
  EXPECT_EQ(Ran.StandardOutput, Message);

  Options.Transfer = SecondTransfer;
  const auto Second = unpack(StagedMode);
  ASSERT_FALSE(HasFailure());
  ASSERT_EQ(Second.Outcome, UnpackOutcome::Unpacked) << Second.Diagnostic;
  ASSERT_EQ(Second.Transfers.size(), 2u);
  EXPECT_EQ(Second.Transfers[0].RVA, Relay->RVA);
  EXPECT_TRUE(Second.Transfers[0].StackBalanced);
  EXPECT_EQ(Second.Transfers[0].Generation, 1u);
  // Code written by generated code is one generation further.
  EXPECT_EQ(Second.Transfers[1].RVA, Original.Entry);
  EXPECT_TRUE(Second.Transfers[1].StackBalanced);
  EXPECT_EQ(Second.Transfers[1].Generation, 2u);
  EXPECT_EQ(Second.EntryRVA, Original.Entry);
  expectOriginalProgram(Second);
}

// Packing replaces ordinary import calls with pure helpers whose continuations
// skip bytes after CALL or restore a pushed register. The recovered sections
// must match the independently linked program byte for byte.
TEST_P(UnpackGenerated, MutatedImportTailCallsAreOrdinaryImportsAgain) {
  if (GetParam().ISA != GuestArchitecture::X64)
    GTEST_SKIP() << X64CallShape;
  for (const uint32_t Mode : {MutatedMode, PaddedCallMode}) {
    SCOPED_TRACE(Mode);
    const auto Result = unpack(Mode);
    ASSERT_FALSE(HasFailure());
    ASSERT_EQ(Result.Outcome, UnpackOutcome::Unpacked) << Result.Diagnostic;
    EXPECT_EQ(Result.EntryRVA, Original.Entry);
    EXPECT_EQ(Result.Source, EntrySource::Transfer);
    EXPECT_EQ(Result.ProcessStop, test::StopObserver);
    expectOriginalProgram(Result, Mode);
  }
}

TEST_P(UnpackGenerated, ExportAddressLoadsAreOrdinaryImportsAgain) {
  if (GetParam().ISA != GuestArchitecture::X64)
    GTEST_SKIP() << X64CallShape;
  for (const uint32_t Mode :
       {AddressMode, PreviousPrefixAddressMode, CallOnlyAddressMode}) {
    SCOPED_TRACE(Mode);
    const auto Result = unpack(Mode);
    ASSERT_FALSE(HasFailure());
    ASSERT_EQ(Result.Outcome, UnpackOutcome::Unpacked) << Result.Diagnostic;
    const auto Image = test::readImage(Result.Image);
    ASSERT_FALSE(HasFailure());
    const uint64_t RVA =
        Mode == PreviousPrefixAddressMode
            ? Original.Exports.at("address_previous_prefix") + 10
            : Original.Exports.at("address_program") + 5;
    EXPECT_EQ(Image.Mapped[RVA], 0x48);
    EXPECT_EQ(Image.Mapped[RVA + 1], 0x8b);
    EXPECT_EQ(Image.Mapped[RVA + 2], 0x1d);
    if (Mode == PreviousPrefixAddressMode)
      EXPECT_EQ(Image.Mapped[RVA - 1], 0x41);
    EXPECT_EQ(Result.ImportRepair.RepairedLoads, 1u);
    const auto Path = Scratch / RebuiltFile;
    test::writeFile(Path, Result.Image);
    const auto Ran = run(Path);
    EXPECT_EQ(Ran.Stop, ProcessStopReason::Exited) << Ran.Diagnostic;
    EXPECT_EQ(Ran.ExitStatus, ExitStatus);
  }
}

TEST_P(UnpackGenerated, ExtendedRegistersLoadOrdinaryImportsAgain) {
  if (GetParam().ISA != GuestArchitecture::X64)
    GTEST_SKIP() << X64CallShape;
  for (const uint32_t Mode : {ExtendedAddressMode, CompactAddressMode}) {
    SCOPED_TRACE(Mode);
    const auto Result = unpack(Mode);
    ASSERT_FALSE(HasFailure());
    ASSERT_EQ(Result.Outcome, UnpackOutcome::Unpacked) << Result.Diagnostic;
    const auto Image = test::readImage(Result.Image);
    ASSERT_FALSE(HasFailure());
    for (unsigned Register = 8; Register != 16; ++Register) {
      SCOPED_TRACE(Register);
      const uint64_t RVA = Original.Exports.at("address_extended_program") +
                           ExtendedLoadOffset +
                           (Register - 8) * ExtendedLoadStride;
      EXPECT_EQ(Image.Mapped[RVA], 0x4c);
      EXPECT_EQ(Image.Mapped[RVA + 1], 0x8b);
      EXPECT_EQ(Image.Mapped[RVA + 2], 0x05 | ((Register & 7) << 3));
      EXPECT_EQ(Image.Mapped[RVA + 7], 0x90);
    }
    EXPECT_EQ(Result.ImportRepair.ObservedLoads, 8u);
    EXPECT_EQ(Result.ImportRepair.RepairedLoads, 8u);
    const auto Path = Scratch / RebuiltFile;
    test::writeFile(Path, Result.Image);
    const auto Ran = run(Path);
    EXPECT_EQ(Ran.Stop, ProcessStopReason::Exited) << Ran.Diagnostic;
    EXPECT_EQ(Ran.ExitStatus, ExitStatus);
  }
}

TEST_P(UnpackGenerated, ImportCallHelpersCannotDiscardPersistentEffects) {
  if (GetParam().ISA != GuestArchitecture::X64)
    GTEST_SKIP() << X64CallShape;
  const auto Result = unpack(ImpureCallMode);
  ASSERT_FALSE(HasFailure());
  ASSERT_EQ(Result.Outcome, UnpackOutcome::Unpacked) << Result.Diagnostic;
  EXPECT_EQ(Result.ImportRepair.RepairedCalls, 2u);
  const auto Path = Scratch / RebuiltFile;
  test::writeFile(Path, Result.Image);
  const auto Ran = run(Path);
  EXPECT_EQ(Ran.Stop, ProcessStopReason::Exited) << Ran.Diagnostic;
  EXPECT_EQ(Ran.ExitStatus, ExitStatus);
}

TEST_P(UnpackGenerated, OpaqueExportCallsAreRepairedBeforeTheExplicitStop) {
  if (GetParam().ISA != GuestArchitecture::X64)
    GTEST_SKIP() << X64CallShape;
  const auto Result = unpack(OpaqueCallMode);
  ASSERT_FALSE(HasFailure());
  ASSERT_EQ(Result.Outcome, UnpackOutcome::Unpacked) << Result.Diagnostic;
  EXPECT_EQ(Result.ImportRepair.ObservedCalls, 1u);
  EXPECT_EQ(Result.ImportRepair.RepairedCalls, 1u);
  EXPECT_EQ(Result.ImportRepair.Stop, "unsupported_service");
  EXPECT_NE(Result.ImportRepair.Diagnostic.find("SetUnhandledExceptionFilter"),
            std::string::npos);
  const auto Image = test::readImage(Result.Image);
  ASSERT_FALSE(HasFailure());
  for (const auto &S : Original.Sections)
    if (S.Name != PackSection)
      EXPECT_EQ(differingBytes(Original, Image, S), 0u) << S.Name;
  const auto Path = Scratch / RebuiltFile;
  test::writeFile(Path, Result.Image);
  const auto Expected = runOriginal(OpaqueCallMode), Actual = run(Path);
  ASSERT_FALSE(HasFailure());
  EXPECT_EQ(Expected.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(Actual.Stop, Expected.Stop);
  EXPECT_EQ(Actual.Diagnostic, Expected.Diagnostic);
  EXPECT_FALSE(Actual.ExitStatus);
  EXPECT_EQ(Actual.Instructions, Expected.Instructions);
}

TEST_P(UnpackGenerated, ExportIdentitySurvivesRebindingAndLateResolution) {
  if (GetParam().ISA != GuestArchitecture::X64)
    GTEST_SKIP() << X64CallShape;
  for (const uint32_t Mode : {ReboundOpaqueCallMode, LateOpaqueCallMode}) {
    SCOPED_TRACE(Mode);
    const auto Result = unpack(Mode);
    ASSERT_FALSE(HasFatalFailure());
    ASSERT_EQ(Result.Outcome, UnpackOutcome::Unpacked) << Result.Diagnostic;
    EXPECT_EQ(Result.ImportRepair.RepairedCalls, 1u);
    EXPECT_EQ(Result.ImportRepair.Stop, "unsupported_service");
    EXPECT_NE(Result.ImportRepair.Diagnostic.find(LateExport),
              std::string::npos);
    const auto Image = test::readImage(Result.Image);
    ASSERT_FALSE(HasFatalFailure());
    const uint64_t RVA = Original.Exports.at("late_program") + LateCallOffset;
    EXPECT_EQ(Image.Mapped[RVA], 0xff);
    EXPECT_EQ(Image.Mapped[RVA + 1], 0x15);
    if (Image.Mapped[RVA] == 0xff && Image.Mapped[RVA + 1] == 0x15) {
      const uint64_t Cell = RVA + 6 +
                            int32_t(llvm::support::endian::read32le(
                                Image.Mapped.data() + RVA + 2));
      const auto Import = llvm::find_if(
          Image.Imports, [&](const auto &I) { return I.Slot == Cell; });
      ASSERT_NE(Import, Image.Imports.end());
      EXPECT_EQ(Import->Module, OpaqueModule);
      EXPECT_EQ(Import->Name, LateExport);
      if (Mode == ReboundOpaqueCallMode)
        EXPECT_EQ(Cell, Original.Exports.at("LateImports"));
    }
    // Independently linked code specifies the import. Only the proven call
    // window may change when a late resolution needs a new loader-owned cell.
    const auto *Program = Original.section(ProgramSection);
    ASSERT_NE(Program, nullptr);
    for (uint64_t At = Program->RVA; At < Program->RVA + Program->VirtualSize;
         ++At)
      if (At < RVA || At >= RVA + 6)
        EXPECT_EQ(Image.Mapped[At], Original.Mapped[At]) << At;
    const auto Path = Scratch / RebuiltFile;
    test::writeFile(Path, Result.Image);
    const auto Expected = runOriginal(Mode), Actual = run(Path);
    ASSERT_FALSE(HasFatalFailure());
    EXPECT_EQ(Expected.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_NE(Expected.Diagnostic.find(LateExport), std::string::npos);
    EXPECT_EQ(Actual.Stop, Expected.Stop);
    EXPECT_EQ(Actual.Diagnostic, Expected.Diagnostic);
    EXPECT_FALSE(Actual.ExitStatus);
  }
}

TEST_P(UnpackGenerated, ImportAddressHelpersCannotDiscardPersistentEffects) {
  if (GetParam().ISA != GuestArchitecture::X64)
    GTEST_SKIP() << X64CallShape;
  const auto Result = unpack(ImpureAddressMode);
  ASSERT_FALSE(HasFailure());
  ASSERT_EQ(Result.Outcome, UnpackOutcome::Unpacked) << Result.Diagnostic;
  const auto Image = test::readImage(Result.Image);
  ASSERT_FALSE(HasFailure());
  const uint64_t RVA = Original.Exports.at("address_program") + 5;
  EXPECT_EQ(Image.Mapped[RVA], 0x5b);
  EXPECT_EQ(Result.ImportRepair.RepairedLoads, 0u);
}

TEST_P(UnpackGenerated,
       ACompletedImportLoadCannotHideALaterUnprovenInvocation) {
  if (GetParam().ISA != GuestArchitecture::X64)
    GTEST_SKIP() << X64CallShape;
  for (const uint32_t Mode : {ChangingAddressMode, UnresolvedAddressMode}) {
    SCOPED_TRACE(Mode);
    const auto Result = unpack(Mode);
    ASSERT_FALSE(HasFailure());
    ASSERT_EQ(Result.Outcome, UnpackOutcome::Unpacked) << Result.Diagnostic;
    const auto Image = test::readImage(Result.Image);
    ASSERT_FALSE(HasFailure());
    const uint64_t RVA = Original.Exports.at("address_twice") + 10;
    EXPECT_EQ(Image.Mapped[RVA], 0x5b);
    EXPECT_EQ(Result.ImportRepair.ObservedLoads, 0u);
    EXPECT_EQ(Result.ImportRepair.RepairedLoads, 0u);
  }
}

TEST_P(UnpackGenerated, ImportAddressHelpersCannotHideOSServiceEffects) {
  if (GetParam().ISA != GuestArchitecture::X64)
    GTEST_SKIP() << X64CallShape;
  const auto Result = unpack(ServiceAddressMode);
  ASSERT_FALSE(HasFailure());
  ASSERT_EQ(Result.Outcome, UnpackOutcome::Unpacked) << Result.Diagnostic;
  const auto Image = test::readImage(Result.Image);
  ASSERT_FALSE(HasFailure());
  const uint64_t RVA = Original.Exports.at("address_program") + 5;
  EXPECT_EQ(Image.Mapped[RVA], 0x5b);
  EXPECT_EQ(Result.ImportRepair.RepairedLoads, 0u);
}

TEST_P(UnpackGenerated,
       MaterializesCompletedTLSStartupAndForwardsOtherNotifications) {
#ifndef NEVERD_WINDOWS_DEFERRED_FIXTURE_DIR
  GTEST_SKIP() << "generated TLS fixture requires Clang and lld-link";
#else
  const auto Path = std::filesystem::path(NEVERD_WINDOWS_DEFERRED_FIXTURE_DIR) /
                    GetParam().Directory / "generated-tls-entry.exe";
  const Image Linked = test::readImage(Path);
  ASSERT_FALSE(HasFailure());
  auto OriginalRun =
      emulateProcess(Path.parent_path() / "generated-tls.exe",
                     ProcessProfile::WindowsPE64, Options.Process);
  ASSERT_TRUE(bool(OriginalRun)) << llvm::toString(OriginalRun.takeError());
  ASSERT_EQ(OriginalRun->Stop, ProcessStopReason::Exited)
      << OriginalRun->Diagnostic;
  ASSERT_EQ(OriginalRun->ExitStatus, 37u);
  auto Result = unpackFile(Path, Options);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Outcome, UnpackOutcome::Unpacked) << Result->Diagnostic;
  EXPECT_EQ(Result->EntryRVA, Linked.Entry);
  EXPECT_EQ(Result->MaterializedTLSCallbacks, 2u);
  EXPECT_EQ(Result->ImportRepair.Stop, "exited")
      << Result->ImportRepair.Diagnostic;
  const auto Output = Scratch / "tls.exe";
  test::writeFile(Output, Result->Image);
  auto Run =
      emulateProcess(Output, ProcessProfile::WindowsPE64, Options.Process);
  ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
  EXPECT_EQ(Run->Stop, ProcessStopReason::Exited) << Run->Diagnostic;
  EXPECT_EQ(Run->ExitStatus, 37u);
#endif
}

TEST_P(UnpackGenerated, DelayImportsRetainLazyLoadingAfterCapture) {
#ifndef NEVERD_UNPACK_GENERATED_FIXTURE_DIR
  GTEST_SKIP() << MissingTools;
#else
  using namespace llvm::support::endian;
  const auto Directory =
      std::filesystem::path(NEVERD_UNPACK_GENERATED_FIXTURE_DIR) /
      GetParam().Directory;
  const auto Linked = test::readImage(Directory / "delay.exe");
  ASSERT_FALSE(HasFailure());
  const auto *Program = Linked.section(".prog");
  const auto *Record = Linked.section(".pay");
  ASSERT_NE(Program, nullptr);
  ASSERT_NE(Record, nullptr);
  ASSERT_LE(Program->VirtualSize, 4096u);
  ASSERT_LE(Program->VirtualSize, Program->FileSize);
  ASSERT_EQ(Program->RVA, Linked.Entry);
  Options.Process.Windows->Modules = {
      {"unpack_delay.dll", Directory / "unpack_delay.dll"}};
  test::writeFile(Scratch / "unpack_delay.dll",
                  test::readFile(Directory / "unpack_delay.dll"));
  for (bool ResolveAll : {false, true}) {
    SCOPED_TRACE(ResolveAll);
    auto Bytes = Linked.File;
    uint8_t *Pay = Bytes.data() + Record->FileOffset;
    write32le(Pay, Program->VirtualSize);
    write32le(Pay + 4, ResolveAll);
    for (uint32_t I = 0; I < Program->VirtualSize; ++I) {
      Pay[8 + I] = Bytes[Program->FileOffset + I] ^ 0xa5;
      Bytes[Program->FileOffset + I] = 0;
    }
    write32le(Bytes.data() + Linked.EntryOffset, Linked.Exports.at("loader"));
    const auto Input = Scratch / "delay-packed.exe";
    const auto Output = Scratch / "delay-rebuilt.exe";
    test::writeFile(Input, Bytes);
    auto OriginalRun =
        emulateProcess(Input, ProcessProfile::WindowsPE64, Options.Process);
    ASSERT_TRUE(bool(OriginalRun)) << llvm::toString(OriginalRun.takeError());
    ASSERT_EQ(OriginalRun->Stop, ProcessStopReason::Exited)
        << OriginalRun->Diagnostic;
    ASSERT_EQ(OriginalRun->ExitStatus, 43u);
    auto Result = unpackFile(Input, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Outcome, UnpackOutcome::Unpacked) << Result->Diagnostic;
    EXPECT_EQ(Result->EntryRVA, Linked.Entry);
    const auto Rebuilt = test::readImage(Result->Image);
    ASSERT_FALSE(HasFailure());
    const auto Delay = Rebuilt.directory(llvm::COFF::DELAY_IMPORT_DESCRIPTOR);
    ASSERT_NE(Delay.Size, 0u);
    const auto *D = Rebuilt.Mapped.data() + Delay.RelativeVirtualAddress;
    const uint64_t Handle = read32le(D + 8), IAT = read32le(D + 12);
    EXPECT_EQ(read64le(Rebuilt.Mapped.data() + Handle), 0u);
    EXPECT_EQ(llvm::count_if(
                  Result->Imports,
                  [](const auto &I) { return I.Module == "unpack_delay.dll"; }),
              ResolveAll ? 2 : 1);
    EXPECT_TRUE(llvm::any_of(Result->Imports, [IAT](const auto &I) {
      return I.SlotRVA == IAT && I.Module == "unpack_delay.dll" &&
             I.Name == "first";
    }));
    if (!ResolveAll)
      EXPECT_EQ(read64le(Rebuilt.Mapped.data() + IAT + 8),
                read64le(Linked.Mapped.data() + IAT + 8));
    else
      EXPECT_TRUE(llvm::any_of(Result->Imports, [IAT](const auto &I) {
        return I.SlotRVA == IAT + 8 && I.Module == "unpack_delay.dll" &&
               I.Name == "second";
      }));
    test::writeFile(Output, Result->Image);
    auto Run =
        emulateProcess(Output, ProcessProfile::WindowsPE64, Options.Process);
    ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
    EXPECT_EQ(Run->Stop, ProcessStopReason::Exited) << Run->Diagnostic;
    EXPECT_EQ(Run->ExitStatus, 43u);
#if defined(_WIN32) && defined(_M_X64)
    if (GetParam().ISA == GuestArchitecture::X64)
      for (const auto &Path : {Input, Output}) {
        const auto Native = Path.string();
        std::string Diagnostic;
        bool Failed = false;
        EXPECT_EQ(llvm::sys::ExecuteAndWait(Native, {Native}, std::nullopt, {},
                                            30, 0, &Diagnostic, &Failed),
                  43)
            << Diagnostic;
        EXPECT_FALSE(Failed) << Diagnostic;
      }
#endif
  }
#endif
}

INSTANTIATE_TEST_SUITE_P(Backends, UnpackGenerated, testing::ValuesIn(Profiles),
                         [](const auto &Info) {
                           return std::string(Info.param.Name);
                         });
} // namespace
} // namespace neverd::unpack
