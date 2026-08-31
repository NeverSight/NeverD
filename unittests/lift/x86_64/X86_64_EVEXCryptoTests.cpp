#include "gtest/gtest.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include <algorithm>
using namespace neverd;
namespace {
std::vector<LowOp> lift(std::initializer_list<uint8_t> Raw) {
  std::vector<uint8_t> Bytes(Raw); Decoder D; EXPECT_TRUE(D.init(Arch::X64));
  DecodedInsn I{}; EXPECT_EQ(D.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, I), (int)Bytes.size());
  std::vector<LowOp> Ops; EXPECT_NO_THROW(D.liftToLow(I, Ops)); return Ops;
}
BinaryImage image(size_t Size) {
  BinaryImage I; I.Arch=Arch::X64; I.Bits=Bitness::Bits64; Segment S;
  S.VA=0x4000; S.Size=Size; S.Flags=SegmentFlags::Readable|SegmentFlags::Writable; S.Data.resize(Size);
  I.Segments.push_back(std::move(S)); return I;
}
void run(NdOpEmulator &E, const std::vector<LowOp> &Ops) { for (const auto &Op:Ops) ASSERT_TRUE(E.step(Op)); }

TEST(X86EVEXCrypto, AesIsLaneLocalAndSupportsHighRegisters) {
  const std::vector<std::pair<std::vector<uint8_t>, unsigned>> Cases{
    {{0x62,0xf2,0x6d,0x48,0xdc,0xcb},1}, {{0x62,0xa2,0x6d,0x40,0xdc,0xcb},17}};
  for (const auto &[Raw,Dst]:Cases) {
    Decoder D; ASSERT_TRUE(D.init(Arch::X64)); DecodedInsn I{};
    ASSERT_EQ(D.decodeOneForLift(Raw.data(),Raw.size(),0x1000,I),(int)Raw.size());
    std::vector<LowOp> Ops; ASSERT_NO_THROW(D.liftToLow(I,Ops));
    BinaryImage Img=image(1); NdOpEmulator E(Img); E.setStrictMode(true);
    E.setRegisterBytes(x86reg::vectorReg(Dst+1),std::vector<uint8_t>(64));
    E.setRegisterBytes(x86reg::vectorReg(Dst+2),std::vector<uint8_t>(64)); run(E,Ops);
    auto R=E.getRegisterBytes(x86reg::vectorReg(Dst)); ASSERT_TRUE(R);
    EXPECT_TRUE(std::all_of(R->begin(),R->end(),[](uint8_t B){return B==0x63;}));
  }
}
TEST(X86EVEXCrypto, AllAesRoundsAndWidths) {
  for(uint8_t Opcode:{uint8_t(0xdc),uint8_t(0xdd),uint8_t(0xde),uint8_t(0xdf)})
    for(uint8_t Length:{uint8_t(0x08),uint8_t(0x28),uint8_t(0x48)}) {
      BinaryImage I=image(1); NdOpEmulator E(I); E.setStrictMode(true);
      E.setRegisterBytes(x86reg::vectorReg(2),std::vector<uint8_t>(64));
      E.setRegisterBytes(x86reg::vectorReg(3),std::vector<uint8_t>(64));
      run(E,lift({0x62,0xf2,0x6d,Length,Opcode,0xcb}));
      auto R=E.getRegisterBytes(x86reg::vectorReg(1)); ASSERT_TRUE(R);
      const size_t Width=Length==0x08?16:(Length==0x28?32:64);
      const uint8_t Expected=Opcode<0xde?0x63:0x52;
      EXPECT_TRUE(std::all_of(R->begin(),R->begin()+Width,[Expected](uint8_t B){return B==Expected;}));
    }
}
TEST(X86EVEXCrypto, PclmulIsLaneLocal) {
  BinaryImage I=image(1); NdOpEmulator E(I); E.setStrictMode(true); std::vector<uint8_t>A(64),B(64);
  for(unsigned L=0;L<4;++L){A[L*16+8]=1;B[L*16+8]=3+L;}
  E.setRegisterBytes(x86reg::vectorReg(2),A);E.setRegisterBytes(x86reg::vectorReg(3),B);
  run(E,lift({0x62,0xf3,0x6d,0x48,0x44,0xcb,0x11})); auto R=E.getRegisterBytes(x86reg::vectorReg(1));ASSERT_TRUE(R);
  for(unsigned L=0;L<4;++L) EXPECT_EQ((*R)[L*16],3+L);
}
TEST(X86EVEXCrypto, PclmulSupportsAllWidths) {
  for(uint8_t Length:{uint8_t(0x08),uint8_t(0x28),uint8_t(0x48)}) {
    BinaryImage I=image(1); NdOpEmulator E(I); E.setStrictMode(true);
    std::vector<uint8_t>A(64),B(64);for(unsigned L=0;L<4;++L){A[L*16]=2;B[L*16]=5;}
    E.setRegisterBytes(x86reg::vectorReg(2),A);E.setRegisterBytes(x86reg::vectorReg(3),B);
    run(E,lift({0x62,0xf3,0x6d,Length,0x44,0xcb,0x00}));
    auto R=E.getRegisterBytes(x86reg::vectorReg(1));ASSERT_TRUE(R);
    const unsigned Lanes=Length==0x08?1:(Length==0x28?2:4);
    for(unsigned L=0;L<Lanes;++L)EXPECT_EQ((*R)[L*16],10);
  }
}
TEST(X86EVEXCrypto, MemorySourceExecutesAfterFullLoad) {
  auto Ops=lift({0x62,0xf2,0x6d,0x48,0xdf,0x08}); BinaryImage I=image(64);
  NdOpEmulator E(I);E.setStrictMode(true);E.setRegister(x86reg::RAX,0x4000);
  E.setRegisterBytes(x86reg::vectorReg(2),std::vector<uint8_t>(64));run(E,Ops);
  auto R=E.getRegisterBytes(x86reg::vectorReg(1));ASSERT_TRUE(R);
  EXPECT_TRUE(std::all_of(R->begin(),R->end(),[](uint8_t B){return B==0x52;}));
}
TEST(X86EVEXCrypto, MemoryFaultDoesNotCommitDestination) {
  auto Ops=lift({0x62,0xf2,0x6d,0x48,0xdf,0x08}); BinaryImage I=image(16); NdOpEmulator E(I);E.setStrictMode(true);
  E.setRegister(x86reg::RAX,0x5000);E.setRegisterBytes(x86reg::vectorReg(2),std::vector<uint8_t>(64));
  std::vector<uint8_t> Old(64,0xa5);E.setRegisterBytes(x86reg::vectorReg(1),Old);bool Failed=false;
  for(const auto &Op:Ops)if(!E.step(Op)){Failed=true;break;} EXPECT_TRUE(Failed);EXPECT_EQ(E.getRegisterBytes(x86reg::vectorReg(1)),Old);
}
TEST(X86EVEXCrypto, ReservedMaskEncodingFailsClosed) {
  const std::vector<uint8_t> Raw{0x62,0xf2,0x6d,0x49,0xdc,0xcb};
  Decoder D;ASSERT_TRUE(D.init(Arch::X64));DecodedInsn I{};
  if(D.decodeOneForLift(Raw.data(),Raw.size(),0x1000,I)!=(int)Raw.size())return;
  std::vector<LowOp> Ops;EXPECT_THROW(D.liftToLow(I,Ops),UnliftedInstruction);
}
} // namespace
