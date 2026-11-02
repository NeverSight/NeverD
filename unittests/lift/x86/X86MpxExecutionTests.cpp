//===- X86MpxExecutionTests.cpp - MPX byte-to-state execution ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Execute original MPX bytes against explicit register/bounds state and an
/// observable memory provider. This does not certify function-level transport.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/lift/X86/X86MpxExecution.h"

#include <array>
#include <cstdint>
#include <iomanip>
#include <vector>

using namespace neverd;

namespace {

using Mode = X86MpxMode;
using Result = X86MpxResult;
using Operation = X86MpxMemoryOperation;
using Outcome = X86MpxMemoryOutcome;
using Segment = X86SegmentRegister;

constexpr va_t Entry = 0x1234567800001000;
constexpr uint64_t Status = 0x76543212;
constexpr std::array<uint8_t, 7> Prefixes = {0,    0x26, 0x2e, 0x36,
                                             0x3e, 0x64, 0x65};

uint64_t mask(Arch A) { return A == Arch::X64 ? UINT64_MAX : UINT32_MAX; }

X86MpxConfiguration configuration() {
  X86MpxConfiguration C;
  C.Supported = true;
  C.OSXSAVE = true;
  C.XCR0 = 0x18;
  C.PrivilegeLevel = 3;
  C.UserConfig = 0x8001;
  C.UserAddressWidthAdjust = 0;
  C.InTransaction = false;
  return C;
}

X86MpxContext context() {
  X86MpxContext C;
  EXPECT_TRUE(C.configure(configuration()));
  for (unsigned R = 0; R != 4; ++R)
    EXPECT_TRUE(C.setBounds(R, {{0x1234000000000100ull + R, UINT64_MAX},
                                {0x5678000000000200ull + R, UINT64_MAX}}));
  C.setStatus(Status);
  return C;
}

std::array<X86MpxBounds, 4> bank(const X86MpxContext &C) {
  return {*C.bounds(0), *C.bounds(1), *C.bounds(2), *C.bounds(3)};
}

class Memory : public X86MpxMemoryProvider {
public:
  std::vector<X86MpxMemoryAccess> Requests;
  std::vector<X86MpxMemoryCompletion> Replies;

  X86MpxMemoryCompletion access(const X86MpxMemoryAccess &A) override {
    Requests.push_back(A);
    if (Requests.size() > Replies.size()) {
      ADD_FAILURE() << "unexpected memory access at " << std::hex << A.Address;
      return {};
    }
    return Replies[Requests.size() - 1];
  }
};

Result execute(const std::vector<uint8_t> &Bytes, Arch A, X86MpxContext &C,
               const X86MpxExecutionInputs &I, Memory &M) {
  const auto R = executeX86MpxInstruction(Bytes, A, Entry, C, I, M);
  EXPECT_EQ(R.Size, Bytes.size());
  return R.Result;
}

TEST(X86MpxExecution, MakeAndAddressChecksUseEffectiveValuesWithoutMemory) {
  for (Arch A : {Arch::X86, Arch::X64})
    for (unsigned S = 0; S != Prefixes.size(); ++S)
      for (bool AddressPrefix : {false, true}) {
        if (A == Arch::X86 && AddressPrefix)
          continue;
        X86MpxExecutionInputs I;
        I.GPR[3] = 0x1234567800000100;
        I.GPR[1] = 0x0000000100000003;
        // [BX + CX * 8 - 128] = 0x1234568000000098 in long mode.
        const uint64_t EA = 0x1234568000000098 & mask(A);
        std::vector<uint8_t> Prefix;
        if (Prefixes[S])
          Prefix.push_back(Prefixes[S]);
        if (AddressPrefix)
          Prefix.push_back(0x67);
        auto Bytes = Prefix;
        Bytes.insert(Bytes.end(), {0xf3, 0x0f, 0x1b, 0x44, 0xcb, 0x80});
        auto C = context();
        auto Wanted = bank(C);
        Memory M;
        EXPECT_EQ(execute(Bytes, A, C, I, M), Result::Completed);
        Wanted[0] = {{*I.GPR[3] & mask(A), mask(A)}, {~EA & mask(A), mask(A)}};
        EXPECT_EQ(bank(C), Wanted);
        EXPECT_EQ(C.status(), Status);
        for (unsigned Kind = 0; Kind != 3; ++Kind)
          for (bool Fault : {false, true}) {
            C = context();
            const uint64_t Limit = Kind == 0 ? EA + Fault : EA - Fault;
            ASSERT_TRUE(
                C.setBounds(2, {{Limit, UINT64_MAX},
                                {Kind == 1 ? ~Limit : Limit, UINT64_MAX}}));
            const auto Before = bank(C);
            Bytes = Prefix;
            Bytes.insert(Bytes.end(),
                         {uint8_t(Kind == 0 ? 0xf3 : 0xf2), 0x0f,
                          uint8_t(Kind == 2 ? 0x1b : 0x1a), 0x54, 0xcb, 0x80});
            EXPECT_EQ(execute(Bytes, A, C, I, M),
                      Fault ? Result::BoundsFault : Result::Completed);
            EXPECT_EQ(bank(C), Before);
            EXPECT_EQ(C.status(), Fault ? 1 : Status);
          }
        EXPECT_TRUE(M.Requests.empty());
      }
}

TEST(X86MpxExecution, RegisterMovesUseBothOpcodeDirectionsAndEveryBoundsAlias) {
  for (Arch A : {Arch::X86, Arch::X64})
    for (unsigned Destination = 0; Destination != 4; ++Destination)
      for (unsigned Source = 0; Source != 4; ++Source)
        for (uint8_t Opcode : {0x1a, 0x1b}) {
          auto C = context();
          auto Wanted = bank(C);
          Wanted[Destination] = Wanted[Source];
          Wanted[Destination].Lower.KnownMask &= mask(A);
          Wanted[Destination].Upper.KnownMask &= mask(A);
          const auto ModRM =
              uint8_t(0xc0 | ((Opcode == 0x1a ? Destination : Source) << 3) |
                      (Opcode == 0x1a ? Source : Destination));
          Memory M;
          EXPECT_EQ(execute({0x66, 0x0f, Opcode, ModRM}, A, C, {}, M),
                    Result::Completed);
          EXPECT_EQ(bank(C), Wanted);
          EXPECT_EQ(C.status(), Status);
          EXPECT_TRUE(M.Requests.empty());
        }
}

TEST(X86MpxExecution, RegisterChecksRetainNativeWidthsAndExtendedSourceBanks) {
  for (Arch A : {Arch::X86, Arch::X64})
    for (unsigned R = 0; R != (A == Arch::X64 ? 16 : 8); ++R)
      for (unsigned Bound = 0; Bound != 4; ++Bound)
        for (unsigned Kind = 0; Kind != 3; ++Kind)
          for (bool Fault : {false, true}) {
            auto C = context();
            X86MpxExecutionInputs I;
            I.GPR[R] = 0xfedcba9800001234;
            const uint64_t Value = *I.GPR[R] & mask(A);
            const uint64_t Limit = Kind == 0 ? Value + Fault : Value - Fault;
            ASSERT_TRUE(
                C.setBounds(Bound, {{Limit, UINT64_MAX},
                                    {Kind == 1 ? ~Limit : Limit, UINT64_MAX}}));
            const auto Before = bank(C);
            std::vector<uint8_t> Bytes{uint8_t(Kind == 0 ? 0xf3 : 0xf2)};
            if (R >= 8)
              Bytes.push_back(0x41);
            Bytes.insert(Bytes.end(), {0x0f, uint8_t(Kind == 2 ? 0x1b : 0x1a),
                                       uint8_t(0xc0 | (Bound << 3) | (R & 7))});
            Memory M;
            EXPECT_EQ(execute(Bytes, A, C, I, M),
                      Fault ? Result::BoundsFault : Result::Completed);
            EXPECT_EQ(bank(C), Before);
            EXPECT_EQ(C.status(), Fault ? 1 : Status);
            EXPECT_TRUE(M.Requests.empty());
          }
}

TEST(X86MpxExecution,
     MemoryMovesRetainDefaultStackSegmentsAndExplicitOverrides) {
  for (Arch A : {Arch::X86, Arch::X64})
    for (unsigned Base : {3u, 4u, 5u, 11u, 12u, 13u}) {
      if (A == Arch::X86 && Base >= 8)
        continue;
      for (unsigned S = 0; S != Prefixes.size(); ++S)
        for (unsigned Form = 0; Form != 3; ++Form) {
          auto C = context();
          auto Wanted = bank(C);
          X86MpxExecutionInputs I;
          I.GPR[Base] = 0x1234567800004000;
          std::vector<uint8_t> Bytes;
          if (Prefixes[S])
            Bytes.push_back(Prefixes[S]);
          if (Form == 2)
            Bytes.push_back(0xf0);
          Bytes.push_back(0x66);
          if (Base >= 8)
            Bytes.push_back(0x41);
          Bytes.insert(Bytes.end(), {0x0f, uint8_t(Form == 0 ? 0x1a : 0x1b),
                                     uint8_t(0x50 | (Base & 7))});
          if ((Base & 7) == 4)
            Bytes.push_back(0x24);
          Bytes.push_back(0x10);
          Memory M;
          M.Replies = {{Outcome::Completed,
                        {0x1111222233334444, 0x5555666677778888, 0}}};
          EXPECT_EQ(execute(Bytes, A, C, I, M), Result::Completed);
          ASSERT_EQ(M.Requests.size(), 1u);
          const auto &Request = M.Requests[0];
          EXPECT_EQ(Request.Operation,
                    Form == 0 ? Operation::LoadBounds : Operation::StoreBounds);
          EXPECT_EQ(Request.Address, 0x1234567800004010 & mask(A));
          EXPECT_EQ(Request.Segment, S                        ? Segment(S - 1)
                                     : Base == 4 || Base == 5 ? Segment::SS
                                                              : Segment::DS);
          EXPECT_EQ(Request.DefaultStack, Base == 4 || Base == 5);
          EXPECT_EQ(Request.usesStackSegment(),
                    A == Arch::X64 ? (Base == 4 || Base == 5) && S < 5
                                   : Request.Segment == Segment::SS);
          EXPECT_EQ(Request.Locked, Form == 2);
          EXPECT_EQ(Request.wordCount(), 2u);
          EXPECT_EQ(Request.wordBytes(), A == Arch::X64 ? 8u : 4u);
          if (Form == 0)
            Wanted[2] = {{0x1111222233334444 & mask(A), mask(A)},
                         {0x5555666677778888 & mask(A), mask(A)}};
          else
            EXPECT_EQ(Request.Words, (std::array<uint64_t, 3>{
                                         Wanted[2].Lower.Value & mask(A),
                                         Wanted[2].Upper.Value & mask(A), 0}));
          EXPECT_EQ(bank(C), Wanted);
        }
    }
}

TEST(X86MpxExecution, TableTranslationIgnoresScaleAndAbsentBaseDisplacement) {
  for (Arch A : {Arch::X86, Arch::X64})
    for (bool NoBase : {false, true})
      for (unsigned Scale = 0; Scale != 4; ++Scale)
        for (bool Store : {false, true})
          for (bool AddressPrefix : {false, true}) {
            if (A == Arch::X86 && AddressPrefix)
              continue;
            auto C = context();
            auto Wanted = bank(C);
            X86MpxExecutionInputs I;
            I.GPR[1] = 0x87654321abcdef01;
            I.GPR[3] = 0x1234000000401900;
            I.SegmentBases[unsigned(Segment::FS)] = 0x1000;
            // With a base: FS:(BX-112) = ...402890. Without one: FS:0.
            const uint64_t Directory = NoBase
                                           ? (A == Arch::X64 ? 0x8000 : 0x8004)
                                           : (A == Arch::X64 ? 0x8020 : 0x9008);
            const uint64_t Table =
                NoBase ? (A == Arch::X64 ? 0x12349000 : 0x12345000)
                       : (A == Arch::X64 ? 0x1234f240 : 0x12347240);
            std::vector<uint8_t> Bytes{0x64};
            if (AddressPrefix)
              Bytes.push_back(0x67);
            Bytes.insert(Bytes.end(),
                         {0x0f, uint8_t(Store ? 0x1b : 0x1a),
                          uint8_t(NoBase ? 0x0c : 0x4c),
                          uint8_t((Scale << 6) | (1 << 3) | (NoBase ? 5 : 3))});
            if (NoBase)
              Bytes.insert(Bytes.end(), {0xff, 0xff, 0xff, 0x7f});
            else
              Bytes.push_back(0x90);
            Memory M;
            M.Replies = {{Outcome::Completed, {0x12345001, 0, 0}},
                         {Outcome::Completed, {0x100, 0x200, *I.GPR[1]}}};
            EXPECT_EQ(execute(Bytes, A, C, I, M), Result::Completed);
            ASSERT_EQ(M.Requests.size(), 2u);
            EXPECT_EQ(M.Requests[0].Operation, Operation::LoadDirectory);
            EXPECT_EQ(M.Requests[0].Address, Directory);
            EXPECT_EQ(M.Requests[1].Operation,
                      Store ? Operation::StoreTable : Operation::LoadTable);
            EXPECT_EQ(M.Requests[1].Address, Table);
            for (const auto &Request : M.Requests) {
              EXPECT_EQ(Request.Segment, Segment::DS);
              EXPECT_FALSE(Request.DefaultStack);
              EXPECT_FALSE(Request.Locked);
            }
            if (Store)
              EXPECT_EQ(
                  M.Requests[1].Words,
                  (std::array<uint64_t, 3>{Wanted[1].Lower.Value & mask(A),
                                           Wanted[1].Upper.Value & mask(A),
                                           *I.GPR[1] & mask(A)}));
            else
              Wanted[1] = {{0x100, mask(A)}, {0x200, mask(A)}};
            EXPECT_EQ(bank(C), Wanted);
            EXPECT_EQ(C.status(), Status);
          }
}

TEST(X86MpxExecution, RipRelativeOffsetsUseTheFullInstructionEndDespite67) {
  for (bool AddressPrefix : {false, true}) {
    auto C = context();
    std::vector<uint8_t> Bytes;
    if (AddressPrefix)
      Bytes.push_back(0x67);
    Bytes.insert(Bytes.end(), {0x66, 0x0f, 0x1a, 0x05, 0xcc, 0xed, 0xff, 0xff});
    Memory M;
    M.Replies = {{Outcome::Completed, {0, 0, 0}}};
    EXPECT_EQ(execute(Bytes, Arch::X64, C, {}, M), Result::Completed);
    ASSERT_EQ(M.Requests.size(), 1u);
    EXPECT_EQ(M.Requests[0].Address, Entry + Bytes.size() - 0x1234);
    EXPECT_EQ(M.Requests[0].Segment, Segment::DS);
  }
}

TEST(X86MpxExecution,
     TableTranslationUsesTheSelectedSegmentAndZeroAbsentIndex) {
  // Deliberately distinct bases identify the segment consumed by translation.
  const std::array<uint64_t, 6> Bases = {0x1000, 0x2020, 0x3040,
                                         0x4060, 0x5080, 0x60a0};
  const std::array<uint64_t, 6> Directory32 = {0x8004, 0x8008, 0x800c,
                                               0x8010, 0x8014, 0x8018};
  const std::array<uint64_t, 6> Table32 = {0x10400, 0x10480, 0x10500,
                                           0x10580, 0x10600, 0x10680};
  for (Arch A : {Arch::X86, Arch::X64})
    for (unsigned Base : {3u, 4u, 5u, 11u, 12u, 13u}) {
      if (A == Arch::X86 && Base >= 8)
        continue;
      for (unsigned S = 0; S != Prefixes.size(); ++S)
        for (bool Store : {false, true}) {
          const auto Selected = S                        ? Segment(S - 1)
                                : Base == 4 || Base == 5 ? Segment::SS
                                                         : Segment::DS;
          const bool NeedsBase = A == Arch::X86 || Selected == Segment::FS ||
                                 Selected == Segment::GS;
          auto C = context();
          X86MpxExecutionInputs I;
          I.GPR[Base] = 0x100;
          for (unsigned R = 0; R != Bases.size(); ++R)
            I.SegmentBases[R] = Bases[R];
          // mod=01 disp8=0, no index; both direct and SIB base forms.
          std::vector<uint8_t> Bytes;
          if (Prefixes[S])
            Bytes.push_back(Prefixes[S]);
          if (Base >= 8)
            Bytes.push_back(0x41);
          Bytes.insert(Bytes.end(), {0x0f, uint8_t(Store ? 0x1b : 0x1a),
                                     uint8_t(0x40 | (Base & 7))});
          if ((Base & 7) == 4)
            Bytes.push_back(0x24);
          Bytes.push_back(0);
          Memory M;
          M.Replies = {{Outcome::Completed, {0x10001, 0, 0}},
                       {Outcome::Completed, {0x123, 0x456, 0}}};
          EXPECT_EQ(execute(Bytes, A, C, I, M), Result::Completed);
          ASSERT_EQ(M.Requests.size(), 2u);
          EXPECT_EQ(M.Requests[0].Address,
                    A == Arch::X86 ? Directory32[unsigned(Selected)] : 0x8000);
          EXPECT_EQ(M.Requests[1].Address, A == Arch::X86
                                               ? Table32[unsigned(Selected)]
                                           : Selected == Segment::FS ? 0x24600
                                           : Selected == Segment::GS ? 0x28680
                                                                     : 0x10400);
          if (Store)
            EXPECT_EQ(M.Requests[1].Words[2], 0u);
          else
            EXPECT_EQ(*C.bounds(0),
                      (X86MpxBounds{{0x123, mask(A)}, {0x456, mask(A)}}));

          // All ignored segment bases can be unknown. A required base must
          // fail before the directory read, never silently become zero.
          C = context();
          I.SegmentBases = {};
          M.Requests.clear();
          EXPECT_EQ(execute(Bytes, A, C, I, M),
                    NeedsBase ? Result::UnknownContext : Result::Completed);
          EXPECT_EQ(M.Requests.size(), NeedsBase ? 0u : 2u);
        }
    }
}

TEST(X86MpxExecution, MissingContextsNeverInventRegisterOrSegmentValues) {
  const std::vector<std::vector<uint8_t>> Forms{
      {0xf3, 0x0f, 0x1b, 0x03}, {0x66, 0x0f, 0x1a, 0x03},
      {0x66, 0x0f, 0x1b, 0x03}, {0xf3, 0x0f, 0x1a, 0xc3},
      {0xf2, 0x0f, 0x1a, 0xc3}, {0xf2, 0x0f, 0x1b, 0xc3},
      {0x0f, 0x1a, 0x03},       {0x0f, 0x1b, 0x03}};
  for (Arch A : {Arch::X86, Arch::X64})
    for (const auto &Bytes : Forms)
      for (unsigned Case = 0; Case != 4; ++Case) {
        auto C = context();
        const auto Before = bank(C);
        auto Config = configuration();
        const auto Wanted = Case == 0   ? Result::UnknownOperand
                            : Case == 1 ? Result::Disabled
                            : Case == 2 ? Result::UnknownContext
                                        : Result::UnsupportedTransaction;
        if (Case == 1)
          Config.Supported = false;
        else if (Case == 2)
          Config = {};
        else if (Case == 3)
          Config.InTransaction = true;
        ASSERT_TRUE(C.configure(Config));
        Memory M;
        EXPECT_EQ(execute(Bytes, A, C, {}, M), Wanted);
        EXPECT_TRUE(M.Requests.empty());
        EXPECT_EQ(bank(C), Before);
        EXPECT_EQ(C.status(), Status);
      }
  for (Arch A : {Arch::X86, Arch::X64})
    for (uint8_t Prefix : {0x64, 0x65}) {
      auto C = context();
      X86MpxExecutionInputs I;
      I.GPR[3] = 0;
      Memory M;
      EXPECT_EQ(execute({Prefix, 0x0f, 0x1a, 0x03}, A, C, I, M),
                Result::UnknownContext);
      EXPECT_TRUE(M.Requests.empty());
    }
}

TEST(X86MpxExecution, RawTableFaultsPublishTheCorrectBankAndBoundsStatus) {
  for (unsigned Bound = 0; Bound != 4; ++Bound)
    for (bool InvalidDirectory : {false, true}) {
      auto C = context();
      auto Wanted = bank(C);
      X86MpxExecutionInputs I;
      I.GPR[3] = 0;
      Memory M;
      M.Replies = {
          {Outcome::Completed, {InvalidDirectory ? 0u : 0x10001u, 0, 0}},
          {Outcome::Fault, {0x1234, 0x5678, 0}}};
      EXPECT_EQ(
          execute({0x0f, 0x1a, uint8_t((Bound << 3) | 3)}, Arch::X64, C, I, M),
          InvalidDirectory ? Result::BoundsFault : Result::MemoryFault);
      if (!InvalidDirectory)
        Wanted[Bound] = {};
      EXPECT_EQ(bank(C), Wanted);
      EXPECT_EQ(C.status(), InvalidDirectory ? 0x8002 : Status);
      EXPECT_EQ(M.Requests.size(), InvalidDirectory ? 1u : 2u);
    }
}

TEST(X86MpxExecution, InstructionSequenceRoundTripsBoundsThroughByteMemory) {
  // This test provider models ordinary flat RAM only. It has no host MPX
  // dependency and is not a provider for native faults or device memory.
  class Ram : public X86MpxMemoryProvider {
  public:
    std::array<uint8_t, 0x20000> Bytes{};
    unsigned Requests = 0;

    void write(uint64_t Address, uint64_t Value, unsigned Width) {
      for (unsigned I = 0; I != Width; ++I)
        Bytes[Address + I] = uint8_t(Value >> (I * 8));
    }
    uint64_t read(uint64_t Address, unsigned Width) const {
      uint64_t Value = 0;
      for (unsigned I = 0; I != Width; ++I)
        Value |= uint64_t(Bytes[Address + I]) << (I * 8);
      return Value;
    }
    X86MpxMemoryCompletion access(const X86MpxMemoryAccess &A) override {
      ++Requests;
      EXPECT_EQ(A.Segment, Segment::DS);
      EXPECT_FALSE(A.Locked);
      const unsigned Width = A.wordBytes();
      if (A.Address > Bytes.size() - A.wordCount() * Width)
        return {Outcome::Fault};
      X86MpxMemoryCompletion R{Outcome::Completed};
      for (unsigned I = 0; I != A.wordCount(); ++I)
        if (A.writesMemory())
          write(A.Address + I * Width, A.Words[I], Width);
        else
          R.Words[I] = read(A.Address + I * Width, Width);
      return R;
    }
  };
  for (Arch A : {Arch::X86, Arch::X64}) {
    auto C = context();
    X86MpxExecutionInputs I;
    I.GPR[3] = 0x100;  // Bounds lower / BNDMK base.
    I.GPR[1] = 0x21;   // BNDMK index, scaled by four, then disp8=-1.
    I.GPR[6] = 0x6000; // BNDMOV spill slot.
    I.GPR[2] = 0x1000; // MIB base.
    I.GPR[7] = 0x1357; // MIB unscaled pointer value.
    I.SegmentBases[unsigned(Segment::DS)] = 0;
    Ram M;
    const unsigned Width = A == Arch::X64 ? 8 : 4;
    const uint64_t Directory = A == Arch::X64 ? 0x8000 : 0x8004;
    const uint64_t Table = A == Arch::X64 ? 0x14000 : 0x10000;
    M.write(Directory, 0x10001, Width);
    M.write(Table + 3 * Width, 0x123456789abcdef0, Width);
    auto Run = [&](std::initializer_list<uint8_t> Bytes) {
      const auto R = executeX86MpxInstruction(Bytes, A, Entry, C, I, M);
      EXPECT_EQ(R.Size, Bytes.size());
      return R.Result;
    };
    const X86MpxBounds Wanted{{0x100, mask(A)},
                              {~uint64_t(0x183) & mask(A), mask(A)}};
    EXPECT_EQ(Run({0xf3, 0x0f, 0x1b, 0x44, 0x8b, 0xff}), Result::Completed);
    EXPECT_EQ(*C.bounds(0), Wanted);
    EXPECT_EQ(M.Requests, 0u);
    EXPECT_EQ(Run({0x66, 0x0f, 0x1b, 0x06}), Result::Completed);
    EXPECT_EQ(M.read(0x6000, Width), Wanted.Lower.Value);
    EXPECT_EQ(M.read(0x6000 + Width, Width), Wanted.Upper.Value);
    EXPECT_EQ(Run({0x66, 0x0f, 0x1a, 0x0e}), Result::Completed);
    EXPECT_EQ(*C.bounds(1), Wanted);
    EXPECT_EQ(Run({0x0f, 0x1b, 0x0c, 0x3a}), Result::Completed);
    EXPECT_EQ(M.read(Table, Width), Wanted.Lower.Value);
    EXPECT_EQ(M.read(Table + Width, Width), Wanted.Upper.Value);
    EXPECT_EQ(M.read(Table + 2 * Width, Width), 0x1357);
    EXPECT_EQ(M.read(Table + 3 * Width, Width), 0x123456789abcdef0 & mask(A));
    EXPECT_EQ(Run({0x0f, 0x1a, 0x14, 0x3a}), Result::Completed);
    EXPECT_EQ(*C.bounds(2), Wanted);
    I.GPR[7] = 0x1358;
    EXPECT_EQ(Run({0x0f, 0x1a, 0x14, 0x3a}), Result::Completed);
    EXPECT_EQ(*C.bounds(2), (X86MpxBounds{{0, mask(A)}, {0, mask(A)}}));
    EXPECT_EQ(M.Requests, 8u);
    I.GPR[0] = 0x100;
    EXPECT_EQ(Run({0xf3, 0x0f, 0x1a, 0xc8}), Result::Completed);
    I.GPR[0] = 0x183;
    EXPECT_EQ(Run({0xf2, 0x0f, 0x1a, 0xc8}), Result::Completed);
    I.GPR[0] = 0x184;
    EXPECT_EQ(Run({0xf2, 0x0f, 0x1a, 0xc8}), Result::BoundsFault);
    EXPECT_EQ(C.status(), 1u);
    EXPECT_EQ(M.Requests, 8u);
    EXPECT_EQ(*C.bounds(0), Wanted);
    EXPECT_EQ(*C.bounds(1), Wanted);
  }
}

TEST(X86MpxExecution,
     MalformedBytesHaveNoEffectsAndArchitecturalNopsNeedNoState) {
  const std::vector<std::vector<uint8_t>> Invalid{
      {},
      {0x66, 0x0f},
      {0x66, 0x0f, 0x1a, 0xe0},
      {0x66, 0x0f, 0x1b, 0xc7},
      {0xf0, 0xf3, 0x0f, 0x1a, 0x03},
      {0xf2, 0xf3, 0x0f, 0x1a, 0xc0}};
  for (Arch A : {Arch::X86, Arch::X64, Arch::AArch64})
    for (const auto &Bytes : Invalid) {
      auto C = context();
      const auto Before = bank(C);
      Memory M;
      const auto R = executeX86MpxInstruction(Bytes, A, Entry, C, {}, M);
      EXPECT_EQ(R.Result, Result::InvalidInput);
      EXPECT_EQ(R.Size, 0);
      EXPECT_EQ(bank(C), Before);
      EXPECT_EQ(C.status(), Status);
      EXPECT_TRUE(M.Requests.empty());
    }
  for (Arch A : {Arch::X86, Arch::X64})
    for (const auto &Bytes :
         std::vector<std::vector<uint8_t>>{{0x0f, 0x1a, 0xff},
                                           {0x0f, 0x1b, 0xff},
                                           {0xf3, 0x0f, 0x1b, 0xff}}) {
      auto C = context();
      const auto Before = bank(C);
      ASSERT_TRUE(C.configure({}));
      Memory M;
      EXPECT_EQ(execute(Bytes, A, C, {}, M), Result::Completed);
      EXPECT_EQ(bank(C), Before);
      EXPECT_EQ(C.status(), Status);
      EXPECT_TRUE(M.Requests.empty());
    }
}

} // namespace
