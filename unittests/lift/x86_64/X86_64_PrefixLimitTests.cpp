//===- X86_64_PrefixLimitTests.cpp - Decoder prefix boundaries -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"

#include <cstddef>
#include <cstdint>
#include <vector>

using namespace neverd;

namespace {
constexpr va_t Entry = 0x1000;

int decode(Decoder &D, unsigned Route, const std::vector<uint8_t> &Bytes,
           DecodedInsn &Out) {
  if (Route == 0)
    return D.decodeOne(Bytes.data(), Bytes.size(), Entry, Out);
  if (Route == 1)
    return D.decodeOneForLift(Bytes.data(), Bytes.size(), Entry, Out);
  return D.decodeOneLight(Bytes.data(), Bytes.size(), Entry, Out);
}

TEST(X86DecodeDetail, PrefixLengthOpcodeIdentityAndRecovery) {
  struct Fence {
    uint8_t ModRM;
    unsigned Id;
  };
  const Fence Fences[] = {
      {0xe8, X86_INS_LFENCE},
      {0xf0, X86_INS_MFENCE},
      {0xf8, X86_INS_SFENCE},
  };
  for (Arch Target : {Arch::X86, Arch::X64})
    for (unsigned Route = 0; Route != 3; ++Route)
      for (bool Detail : {false, true})
        for (bool Text : {false, true}) {
          Decoder D;
          ASSERT_TRUE(D.init(Target));
          D.setDetail(Detail);
          D.setText(Text);
          for (const auto &F : Fences)
            for (size_t Prefixes :
                 {size_t{0}, size_t{1}, size_t{11}, size_t{12}, size_t{13},
                  size_t{14}, size_t{15}, size_t{16}, size_t{4096}}) {
              SCOPED_TRACE(testing::Message()
                           << unsigned(Target) << ": " << Route << ": "
                           << Detail << ": " << Text << ": " << F.Id << ": "
                           << Prefixes);
              std::vector<uint8_t> Bytes(Prefixes, 0x66);
              Bytes.insert(Bytes.end(), {0x0f, 0xae, F.ModRM});
              // 66 is mandatory for TPAUSE in the /6 encoding. It must not
              // be stripped as an ignored MFENCE prefix (Intel SDM, TPAUSE).
              const unsigned ExpectedId =
                  Prefixes && F.Id == X86_INS_MFENCE ? X86_INS_TPAUSE : F.Id;
              DecodedInsn Insn{};
              const int Size = decode(D, Route, Bytes, Insn);
              if (Prefixes <= 12) {
                ASSERT_EQ(Size, Prefixes + 3);
                ASSERT_NE(Insn.Raw, nullptr);
                EXPECT_EQ(Insn.Id, ExpectedId);
                EXPECT_EQ(Insn.Raw->address, Entry);
                EXPECT_EQ(Insn.Raw->size, Size);
                EXPECT_EQ(std::vector<uint8_t>(Insn.Raw->bytes,
                                               Insn.Raw->bytes + Size),
                          Bytes);
                if (Detail) {
                  ASSERT_NE(Insn.Raw->detail, nullptr);
                  if (Text)
                    EXPECT_EQ(Insn.Raw->detail->x86.op_count,
                              ExpectedId == X86_INS_TPAUSE ? 1 : 0);
                }
              } else {
                EXPECT_EQ(Size, 0);
              }

              DecodedInsn Next{};
              ASSERT_EQ(decode(D, Route, {0x90}, Next), 1);
              EXPECT_EQ(Next.Id, X86_INS_NOP);
              ASSERT_NE(Next.Raw, nullptr);
              EXPECT_EQ(Next.Raw->address, Entry);
              EXPECT_EQ(Next.Raw->bytes[0], 0x90);
            }
        }
}
} // namespace
