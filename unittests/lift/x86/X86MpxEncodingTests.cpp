//===- X86MpxEncodingTests.cpp - Original MPX operand regressions -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check MPX addressing and exact instruction extents independently of host
/// enablement. Decode support does not admit the unfinished stateful lifter.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/lift/X86/X86MpxEncoding.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

using namespace neverd;

namespace {

TEST(X86MpxEncoding, OriginalControlTransfersOwnBoundsInitialization) {
  using Effect = X86MpxBranch;
  for (Arch Target : {Arch::X86, Arch::X64}) {
    for (const auto &Bytes :
         std::vector<std::vector<uint8_t>>{{0xe8, 0, 0, 0, 0},
                                           {0xe9, 0, 0, 0, 0},
                                           {0xc3},
                                           {0xc2, 8, 0},
                                           {0xff, 0xd0},
                                           {0xff, 0x20},
                                           {0x74, 0},
                                           {0x0f, 0x85, 0, 0, 0, 0}}) {
      EXPECT_EQ(x86MpxBranchEffect(Bytes, Target), Effect::Initialize);
      auto Bnd = Bytes;
      Bnd.insert(Bnd.begin(), 0xf2);
      EXPECT_EQ(x86MpxBranchEffect(Bnd, Target), Effect::Preserve);
      Bnd.insert(Bnd.begin(), 0xf3);
      EXPECT_FALSE(x86MpxBranchEffect(Bnd, Target));
    }
    for (const auto &Bytes : std::vector<std::vector<uint8_t>>{{0xeb, 0},
                                                               {0xe0, 0},
                                                               {0xe1, 0},
                                                               {0xe2, 0},
                                                               {0xe3, 0},
                                                               {0xcb},
                                                               {0xca, 0, 0},
                                                               {0xcf},
                                                               {0xff, 0x18},
                                                               {0xff, 0x28}})
      EXPECT_EQ(x86MpxBranchEffect(Bytes, Target), Effect::Preserve);
    EXPECT_FALSE(x86MpxBranchEffect({}, Target));
    EXPECT_FALSE(x86MpxBranchEffect({0xf0, 0xff, 0xd0}, Target));
    EXPECT_FALSE(x86MpxBranchEffect({0x90}, Target));
    for (unsigned CC = 0; CC != 16; ++CC) {
      EXPECT_EQ(x86MpxBranchEffect({uint8_t(0x70 + CC), 0}, Target),
                Effect::Initialize);
      EXPECT_EQ(
          x86MpxBranchEffect({0x0f, uint8_t(0x80 + CC), 0, 0, 0, 0}, Target),
          Effect::Initialize);
    }
  }
}

constexpr va_t Entry = 0x1234567800001000;

struct Fixture {
  std::vector<uint8_t> Bytes;
  unsigned Id;
  unsigned BoundOperand;
  x86_reg Bound;
  x86_reg Base32;
  x86_reg Base64;
  x86_reg Index32;
  x86_reg Index64;
  unsigned Scale;
  int64_t Displacement;
  bool Store;
};

const std::array<Fixture, 8> MemoryFixtures = {{
    {{0xf3, 0x0f, 0x1a, 0x54, 0x8b, 0x80},
     X86_INS_BNDCL,
     0,
     X86_REG_BND2,
     X86_REG_EBX,
     X86_REG_RBX,
     X86_REG_ECX,
     X86_REG_RCX,
     4,
     -128,
     false},
    {{0xf2, 0x0f, 0x1a, 0x4c, 0x77, 0x7f},
     X86_INS_BNDCU,
     0,
     X86_REG_BND1,
     X86_REG_EDI,
     X86_REG_RDI,
     X86_REG_ESI,
     X86_REG_RSI,
     2,
     127,
     false},
    {{0xf2, 0x0f, 0x1b, 0x5d, 0xff},
     X86_INS_BNDCN,
     0,
     X86_REG_BND3,
     X86_REG_EBP,
     X86_REG_RBP,
     X86_REG_INVALID,
     X86_REG_INVALID,
     1,
     -1,
     false},
    {{0xf3, 0x0f, 0x1b, 0x04, 0xcb},
     X86_INS_BNDMK,
     0,
     X86_REG_BND0,
     X86_REG_EBX,
     X86_REG_RBX,
     X86_REG_ECX,
     X86_REG_RCX,
     8,
     0,
     false},
    {{0x66, 0x0f, 0x1a, 0x54, 0x24, 0x10},
     X86_INS_BNDMOV,
     0,
     X86_REG_BND2,
     X86_REG_ESP,
     X86_REG_RSP,
     X86_REG_INVALID,
     X86_REG_INVALID,
     1,
     16,
     false},
    {{0x66, 0x0f, 0x1b, 0x9b, 0x78, 0x56, 0x34, 0x12},
     X86_INS_BNDMOV,
     1,
     X86_REG_BND3,
     X86_REG_EBX,
     X86_REG_RBX,
     X86_REG_INVALID,
     X86_REG_INVALID,
     1,
     0x12345678,
     true},
    {{0x0f, 0x1a, 0x54, 0xcb, 0x40},
     X86_INS_BNDLDX,
     0,
     X86_REG_BND2,
     X86_REG_EBX,
     X86_REG_RBX,
     X86_REG_ECX,
     X86_REG_RCX,
     8,
     64,
     false},
    {{0x0f, 0x1b, 0x54, 0xcb, 0x40},
     X86_INS_BNDSTX,
     1,
     X86_REG_BND2,
     X86_REG_EBX,
     X86_REG_RBX,
     X86_REG_ECX,
     X86_REG_RCX,
     8,
     64,
     true},
}};

int decode(Decoder &D, const std::vector<uint8_t> &Bytes, unsigned Route,
           DecodedInsn &I) {
  if (Route == 0)
    return D.decodeOne(Bytes.data(), Bytes.size(), Entry, I);
  if (Route == 1)
    return D.decodeOneForLift(Bytes.data(), Bytes.size(), Entry, I);
  return D.decodeOneLight(Bytes.data(), Bytes.size(), Entry, I);
}

TEST(X86MpxEncoding, MemoryOperandsAgreeAcrossDecodeRoutes) {
  unsigned Cases = 0;
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto &F : MemoryFixtures)
      for (const auto Segment : {0, 0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65})
        for (const bool AddressPrefix : {false, true}) {
          if (A == Arch::X86 && AddressPrefix)
            continue;
          auto Bytes = F.Bytes;
          if (Segment)
            Bytes.insert(Bytes.begin(), Segment);
          if (AddressPrefix)
            Bytes.insert(Bytes.begin(), 0x67);
          for (unsigned Route = 0; Route != 3; ++Route) {
            SCOPED_TRACE(::testing::Message()
                         << unsigned(A) << '/' << F.Id << '/' << Segment << '/'
                         << AddressPrefix << '/' << Route);
            Decoder D;
            ASSERT_TRUE(D.init(A));
            DecodedInsn I{};
            ASSERT_EQ(decode(D, Bytes, Route, I), Bytes.size());
            ASSERT_EQ(I.Id, F.Id);
            ASSERT_NE(I.Raw->detail, nullptr);
            const auto &X = I.Raw->detail->x86;
            EXPECT_EQ(X.addr_size, A == Arch::X64 ? 8 : 4);
            EXPECT_EQ(X.prefix[3], AddressPrefix ? 0x67 : 0);
            EXPECT_EQ(X.prefix[1], Segment);
            ASSERT_EQ(X.op_count, 2);
            EXPECT_EQ(X.operands[F.BoundOperand].reg, F.Bound);
            const auto &M = X.operands[1 - F.BoundOperand];
            ASSERT_EQ(M.type, X86_OP_MEM);
            EXPECT_EQ(M.mem.base, A == Arch::X64 ? F.Base64 : F.Base32);
            EXPECT_EQ(M.mem.index, A == Arch::X64 ? F.Index64 : F.Index32);
            EXPECT_EQ(M.mem.scale, F.Scale);
            EXPECT_EQ(M.mem.disp, F.Displacement);
            EXPECT_EQ(M.access, F.Store ? CS_AC_WRITE : CS_AC_READ);
            if (F.Id == X86_INS_BNDMOV)
              EXPECT_EQ(M.size, A == Arch::X64 ? 16 : 8);
            EXPECT_FALSE(D.isAlignmentNop(I));
            std::vector<LowOp> Ops;
            EXPECT_THROW(D.liftToLow(I, Ops), UnliftedInstruction);
            EXPECT_TRUE(Ops.empty());
            ++Cases;
          }
        }
  EXPECT_EQ(Cases, 504u);
  RecordProperty("memory_decode_cases", Cases);
}

TEST(X86MpxEncoding, IgnoredAddressPrefixKeepsHighRegistersAndRip) {
  struct Case {
    std::vector<uint8_t> Bytes;
    x86_reg Base;
    x86_reg Index;
    unsigned Scale;
    int64_t Disp;
    const char *Text;
  };
  const Case Cases[] = {
      {{0x67, 0xf3, 0x43, 0x0f, 0x1a, 0x54, 0xcd, 0x80},
       X86_REG_R13,
       X86_REG_R9,
       8,
       -128,
       "bnd2, [r13 + r9*8 - 0x80]"},
      {{0x67, 0x66, 0x49, 0x0f, 0x1a, 0x05, 0xff, 0xff, 0xff, 0xff},
       X86_REG_RIP,
       X86_REG_INVALID,
       1,
       -1,
       "bnd0, xmmword ptr [rip - 0x1]"},
      {{0x67, 0xf2, 0x0f, 0x1b, 0x05, 0x00, 0x00, 0x00, 0x80},
       X86_REG_RIP,
       X86_REG_INVALID,
       1,
       INT32_MIN,
       "bnd0, [rip - 0x80000000]"},
      {{0x67, 0x65, 0x43, 0x0f, 0x1a, 0x04, 0xe5, 0x78, 0x56, 0x34, 0x12},
       X86_REG_INVALID,
       X86_REG_R12,
       8,
       0x12345678,
       "bnd0, gs:[r12*8 + 0x12345678]"},
  };
  Decoder D;
  ASSERT_TRUE(D.init(Arch::X64));
  for (const auto &C : Cases) {
    DecodedInsn I{};
    ASSERT_EQ(D.decodeOne(C.Bytes.data(), C.Bytes.size(), Entry, I),
              C.Bytes.size());
    const auto &X = I.Raw->detail->x86;
    ASSERT_EQ(X.op_count, 2);
    EXPECT_EQ(X.addr_size, 8);
    const auto &M = X.operands[1].mem;
    EXPECT_EQ(M.base, C.Base);
    EXPECT_EQ(M.index, C.Index);
    EXPECT_EQ(M.scale, C.Scale);
    EXPECT_EQ(M.disp, C.Disp);
    EXPECT_STREQ(I.Raw->op_str, C.Text);
  }
}

TEST(X86MpxEncoding, RegisterWidthsAndMoveDirectionAreModeOwned) {
  constexpr std::array<x86_reg, 16> GPR64 = {
      X86_REG_RAX, X86_REG_RCX, X86_REG_RDX, X86_REG_RBX,
      X86_REG_RSP, X86_REG_RBP, X86_REG_RSI, X86_REG_RDI,
      X86_REG_R8,  X86_REG_R9,  X86_REG_R10, X86_REG_R11,
      X86_REG_R12, X86_REG_R13, X86_REG_R14, X86_REG_R15};
  constexpr std::array<x86_reg, 8> GPR32 = {
      X86_REG_EAX, X86_REG_ECX, X86_REG_EDX, X86_REG_EBX,
      X86_REG_ESP, X86_REG_EBP, X86_REG_ESI, X86_REG_EDI};
  unsigned Cases = 0;
  for (const auto A : {Arch::X86, Arch::X64})
    for (unsigned Bank = 0; Bank != 4; ++Bank)
      for (unsigned RM = 0; RM != (A == Arch::X64 ? 16u : 8u); ++RM)
        for (unsigned Kind = 0; Kind != 5; ++Kind)
          for (bool AddressPrefix : {false, true})
            for (bool RexW : {false, true}) {
              if ((RexW && A == Arch::X86) || (Kind >= 3 && RM >= 4))
                continue;
              std::vector<uint8_t> Bytes;
              if (AddressPrefix)
                Bytes.push_back(0x67);
              Bytes.push_back(Kind == 0 ? 0xf3 : Kind < 3 ? 0xf2 : 0x66);
              if (A == Arch::X64 && (RM >= 8 || RexW))
                Bytes.push_back(0x40 | (RM >= 8 ? 1 : 0) | (RexW ? 8 : 0));
              Bytes.push_back(0x0f);
              Bytes.push_back(Kind == 2 || Kind == 4 ? 0x1b : 0x1a);
              Bytes.push_back(0xc0 | (Bank << 3) | (RM & 7));
              const auto E = decodeX86MpxEncoding(Bytes, A);
              ASSERT_TRUE(E);
              const auto &X = E->Detail;
              EXPECT_EQ(E->Size, Bytes.size());
              EXPECT_EQ(X.addr_size, A == Arch::X64 ? 8 : 4);
              const unsigned B = Kind == 4 ? 1 : 0;
              EXPECT_EQ(X.operands[B].reg, X86_REG_BND0 + Bank);
              EXPECT_EQ(X.operands[B].size, 16);
              EXPECT_EQ(X.operands[1 - B].reg, Kind >= 3 ? X86_REG_BND0 + RM
                                               : A == Arch::X64 ? GPR64[RM]
                                                                : GPR32[RM]);
              EXPECT_EQ(X.operands[1 - B].size, Kind >= 3        ? 16
                                                : A == Arch::X64 ? 8
                                                                 : 4);
              if (Kind >= 3) {
                EXPECT_EQ(X.operands[0].access, CS_AC_WRITE);
                EXPECT_EQ(X.operands[1].access, CS_AC_READ);
              }
              ++Cases;
            }
  EXPECT_EQ(Cases, 1152u);
  RecordProperty("register_encoding_cases", Cases);
}

TEST(X86MpxEncoding, LockedStoresDecodeWithoutAdmittingOrdinaryStores) {
  for (const auto A : {Arch::X86, Arch::X64})
    for (unsigned Bank = 0; Bank != 4; ++Bank)
      for (unsigned Route = 0; Route != 3; ++Route)
        for (bool Text : {false, true}) {
          Decoder D;
          ASSERT_TRUE(D.init(A));
          D.setText(Text);
          const std::vector<uint8_t> Bytes = {
              0xf0, 0x66, 0x0f, 0x1b, uint8_t(0x43 | (Bank << 3)), 0x08};
          DecodedInsn I{};
          ASSERT_EQ(decode(D, Bytes, Route, I), Bytes.size());
          EXPECT_EQ(I.Id, X86_INS_BNDMOV);
          EXPECT_FALSE(D.isAlignmentNop(I));
          EXPECT_EQ(I.Raw->illegal, false);
          if (!Text) {
            EXPECT_STREQ(I.Raw->mnemonic, "");
            EXPECT_STREQ(I.Raw->op_str, "");
            continue;
          }
          EXPECT_STREQ(I.Raw->mnemonic, "lock bndmov");
          const auto &X = I.Raw->detail->x86;
          EXPECT_EQ(X.prefix[0], 0xf0);
          EXPECT_EQ(X.prefix[2], 0x66);
          EXPECT_EQ(X.operands[0].access, CS_AC_WRITE);
          EXPECT_EQ(X.operands[0].size, A == Arch::X64 ? 16 : 8);
          EXPECT_EQ(X.operands[1].reg, X86_REG_BND0 + Bank);
          std::vector<LowOp> Ops;
          EXPECT_THROW(D.liftToLow(I, Ops), UnliftedInstruction);
          EXPECT_TRUE(Ops.empty());
        }
}

TEST(X86MpxEncoding, ArchitecturalRegisterNopsConsumeTheEntireEncoding) {
  unsigned Cases = 0;
  for (const auto A : {Arch::X86, Arch::X64}) {
    Decoder D;
    ASSERT_TRUE(D.init(A));
    for (unsigned Kind = 0; Kind != 3; ++Kind)
      for (unsigned ModRM = 0xc0; ModRM <= 0xff; ++ModRM)
        for (unsigned Prefixes = 0; Prefixes != 4; ++Prefixes)
          for (unsigned Rex : {0u, 0x40u, 0x4fu}) {
            if (A == Arch::X86 && Rex)
              continue;
            std::vector<uint8_t> Bytes;
            if (Prefixes & 1)
              Bytes.push_back(0x67);
            if (Prefixes & 2)
              Bytes.push_back(0x65);
            if (Kind == 2)
              Bytes.push_back(0xf3);
            if (Rex)
              Bytes.push_back(Rex);
            Bytes.insert(Bytes.end(),
                         {0x0f, uint8_t(Kind ? 0x1b : 0x1a), uint8_t(ModRM)});
            const auto Length = Bytes.size();
            Bytes.push_back(0xcc);
            for (unsigned Route = 0; Route != 3; ++Route) {
              DecodedInsn I{};
              ASSERT_EQ(decode(D, Bytes, Route, I), Length);
              EXPECT_EQ(I.Id, X86_INS_NOP);
              EXPECT_TRUE(D.isAlignmentNop(I));
              EXPECT_EQ(I.Raw->detail->x86.op_count, 0);
              std::vector<LowOp> Ops;
              ASSERT_NO_THROW(D.liftToLow(I, Ops));
              ASSERT_EQ(Ops.size(), 1u);
              EXPECT_EQ(Ops[0].Opcode, NdOp::NOP);
              ++Cases;
            }
          }
  }
  EXPECT_EQ(Cases, 9216u);
  RecordProperty("architectural_nop_cases", Cases);
}

TEST(X86MpxEncoding, TableOperandsRetainAbsentBaseIndexAndIgnoredScale) {
  // A missing MIB base means zero before segmentation, even with a nonzero
  // displacement. Keep the absent base distinct from an absolute address.
  for (const auto A : {Arch::X86, Arch::X64})
    for (unsigned Scale = 0; Scale != 4; ++Scale)
      for (const bool Index : {false, true})
        for (const bool Store : {false, true}) {
          std::vector<uint8_t> Bytes = {
              0x64,
              0x0f,
              uint8_t(Store ? 0x1b : 0x1a),
              0x14,
              uint8_t((Scale << 6) | (Index ? 0x0d : 0x25)),
              0x78,
              0x56,
              0x34,
              0x12};
          const auto E = decodeX86MpxEncoding(Bytes, A);
          ASSERT_TRUE(E);
          const auto &M = E->Detail.operands[Store ? 0 : 1].mem;
          EXPECT_EQ(M.segment, X86_REG_FS);
          EXPECT_EQ(M.base, X86_REG_INVALID);
          EXPECT_EQ(M.index, !Index           ? X86_REG_INVALID
                             : A == Arch::X64 ? X86_REG_RCX
                                              : X86_REG_ECX);
          EXPECT_EQ(M.scale, 1 << Scale);
          EXPECT_EQ(M.disp, 0x12345678);
        }
}

TEST(X86MpxEncoding, EverySibAddressAgreesWithUnprefixedNativeWidthAddressing) {
  unsigned Cases = 0;
  for (const auto A : {Arch::X86, Arch::X64}) {
    Decoder D;
    ASSERT_TRUE(D.init(A));
    for (unsigned Mod = 0; Mod != 3; ++Mod)
      for (unsigned SIB = 0; SIB != 256; ++SIB)
        for (unsigned Extension = 0; Extension != 4; ++Extension)
          for (bool Prefix67 : {false, true}) {
            if (A == Arch::X86 && (Extension || Prefix67))
              continue;
            std::vector<uint8_t> Tail = {uint8_t((Mod << 6) | 0x1c),
                                         uint8_t(SIB)};
            if (Mod == 1)
              Tail.push_back(0x80);
            else if (Mod == 2 || (SIB & 7) == 5)
              Tail.insert(Tail.end(), {0xef, 0xcd, 0xab, 0x89});
            // The pinned decoder independently supplies ordinary MOV's
            // complete native-width address. MPX ignores 67H in long mode.
            std::vector<uint8_t> Reference;
            if (A == Arch::X64)
              Reference.push_back(0x48 | Extension);
            Reference.push_back(0x8b);
            Reference.insert(Reference.end(), Tail.begin(), Tail.end());
            DecodedInsn I{};
            ASSERT_EQ(decode(D, Reference, 0, I), Reference.size());
            ASSERT_EQ(I.Id, X86_INS_MOV);
            const auto Expected = I.Raw->detail->x86.operands[1].mem;
            // Capstone sometimes prints RIZ/EIZ for the absent SIB index.
            // These encode a constant zero, not a GPR input to MPX.
            const auto ExpectedIndex =
                Expected.index == X86_REG_RIZ || Expected.index == X86_REG_EIZ
                    ? X86_REG_INVALID
                    : Expected.index;
            std::vector<uint8_t> Bytes;
            if (Prefix67)
              Bytes.push_back(0x67);
            Bytes.push_back(0x66);
            if (A == Arch::X64)
              Bytes.push_back(0x40 | Extension);
            Bytes.insert(Bytes.end(), {0x0f, 0x1a});
            Bytes.insert(Bytes.end(), Tail.begin(), Tail.end());
            ASSERT_EQ(decode(D, Bytes, 0, I), Bytes.size());
            ASSERT_EQ(I.Id, X86_INS_BNDMOV);
            const auto &X = I.Raw->detail->x86;
            const auto &Actual = X.operands[1].mem;
            EXPECT_EQ(Actual.base, Expected.base);
            EXPECT_EQ(Actual.index, ExpectedIndex);
            EXPECT_EQ(Actual.scale, Expected.scale);
            EXPECT_EQ(Actual.disp, Expected.disp);
            EXPECT_EQ(X.operands[0].reg, X86_REG_BND3);
            EXPECT_EQ(X.sib, SIB);
            EXPECT_EQ(X.sib_base, Expected.base);
            EXPECT_EQ(X.sib_index, ExpectedIndex);
            EXPECT_EQ(X.prefix[3], Prefix67 ? 0x67 : 0);
            EXPECT_EQ(X.addr_size, A == Arch::X64 ? 8 : 4);
            ++Cases;
          }
  }
  EXPECT_EQ(Cases, 6912u);
  RecordProperty("sib_differential_cases", Cases);
}

TEST(X86MpxEncoding, InvalidOrAmbiguousEncodingsDoNotAcquireAValidShape) {
  const std::vector<std::vector<uint8_t>> Invalid = {
      {0xf3, 0x0f, 0x1a, 0xe0},       // BND4.
      {0x66, 0x0f, 0x1a, 0xc4},       // Source BND4.
      {0xf0, 0x66, 0x0f, 0x1a, 0x00}, // LOCK load.
      {0xf0, 0x66, 0x0f, 0x1b, 0xc0}, // LOCK register move.
      {0xf0, 0x0f, 0x1a, 0xc0},       // LOCK NOP/table instruction.
      {0xf0, 0x0f, 0x1b, 0x00},
      {0xf0, 0xf3, 0x0f, 0x1b, 0x00},
      {0xf2, 0xf3, 0x0f, 0x1a, 0xc0},
      {0xf3, 0x66, 0x0f, 0x1a, 0xc0},
      {0x66, 0xf2, 0x0f, 0x1a, 0xc0},
      {0x67, 0x67, 0x0f, 0x1a, 0xc0},
      {0x64, 0x65, 0x0f, 0x1b, 0xc0},
      {0xc4, 0xe1, 0x78, 0x1a, 0xc0},
      {0x62, 0xf1, 0x7c, 0x08, 0x1a, 0xc0},
      {0xd5, 0x80, 0x1a, 0xc0},
  };
  for (const auto &Bytes : Invalid)
    for (const auto A : {Arch::X86, Arch::X64})
      EXPECT_FALSE(decodeX86MpxEncoding(Bytes, A));
  for (const auto &F : MemoryFixtures) {
    auto Bytes = F.Bytes;
    Bytes.insert(Bytes.begin(), 0x67);
    EXPECT_FALSE(decodeX86MpxEncoding(Bytes, Arch::X86));
  }
  for (auto Prefix : {0u, 0xf3u}) {
    std::vector<uint8_t> Bytes = {0x0f, 0x1b, 0x05, 0, 0, 0, 0};
    if (Prefix)
      Bytes.insert(Bytes.begin(), Prefix);
    EXPECT_FALSE(decodeX86MpxEncoding(Bytes, Arch::X64));
    EXPECT_TRUE(decodeX86MpxEncoding(Bytes, Arch::X86));
  }
  for (uint8_t Rex : {0x44, 0x4c})
    EXPECT_FALSE(
        decodeX86MpxEncoding({0xf3, Rex, 0x0f, 0x1a, 0xc0}, Arch::X64));
  EXPECT_FALSE(decodeX86MpxEncoding({0xf3, 0x0f, 0x1a, 0xc0}, Arch::ARM));
}

TEST(X86MpxEncoding, TruncationAndLengthLimitsNeverConsumeFollowingBytes) {
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto &F : MemoryFixtures) {
      for (size_t Length = 0; Length != F.Bytes.size(); ++Length)
        EXPECT_FALSE(decodeX86MpxEncoding(
            llvm::ArrayRef(F.Bytes).take_front(Length), A));
      auto Bytes = F.Bytes;
      Bytes.insert(Bytes.end(), {0xcc, 0x90});
      const auto E = decodeX86MpxEncoding(Bytes, A);
      ASSERT_TRUE(E);
      EXPECT_EQ(E->Size, F.Bytes.size());
    }
  std::vector<uint8_t> Long(12, 0x40);
  Long.insert(Long.end(), {0x0f, 0x1a, 0xc0});
  ASSERT_EQ(Long.size(), 15u);
  ASSERT_TRUE(decodeX86MpxEncoding(Long, Arch::X64));
  Long.insert(Long.begin(), 0x40);
  EXPECT_FALSE(decodeX86MpxEncoding(Long, Arch::X64));
  // A REX before a legacy prefix has no register-extension effect.
  auto Cancelled =
      decodeX86MpxEncoding({0x44, 0xf3, 0x0f, 0x1a, 0xc0}, Arch::X64);
  ASSERT_TRUE(Cancelled);
  EXPECT_EQ(Cancelled->Detail.rex, 0);
  EXPECT_EQ(Cancelled->Detail.operands[0].reg, X86_REG_BND0);
}

TEST(X86MpxEncoding, LightDecodingDoesNotReusePriorOperandsOrText) {
  Decoder D;
  ASSERT_TRUE(D.init(Arch::X64));
  for (bool Detail : {true, false})
    for (bool Text : {true, false}) {
      D.setDetail(Detail);
      D.setText(Text);
      for (const auto &Bytes :
           {std::vector<uint8_t>{0x67, 0xf3, 0x0f, 0x1a, 0x54, 0x8b, 0x80},
            std::vector<uint8_t>{0xf0, 0x66, 0x0f, 0x1b, 0x00},
            std::vector<uint8_t>{0xf3, 0x67, 0x0f, 0x1b, 0xc0}}) {
        DecodedInsn I{};
        ASSERT_EQ(D.decodeOneLight(Bytes.data(), Bytes.size(), Entry, I),
                  Bytes.size());
        if (!Text) {
          EXPECT_STREQ(I.Raw->mnemonic, "");
          EXPECT_STREQ(I.Raw->op_str, "");
        }
        if (Detail && Text) {
          ASSERT_NE(I.Raw->detail, nullptr);
          EXPECT_EQ(I.Raw->detail->x86.addr_size, 8);
          EXPECT_EQ(I.Raw->detail->x86.op_count, I.Id == X86_INS_NOP ? 0 : 2);
        }
      }
    }
}

} // namespace
