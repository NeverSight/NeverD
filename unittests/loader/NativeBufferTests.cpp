#include "../web/NativeFixture.h"
#include "gtest/gtest.h"

#include "neverd/support/BinaryLoading.h"

#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"

namespace neverd {
namespace {

TEST(NativeBuffer, FileLoaderChoicesNeverOverrideCapturedNativeBytes) {
  const auto Bytes = web::test::nativeELF();
  const llvm::MemoryBufferRef Buffer(Bytes, "/nonexistent/captured-input.exe");
  for (const auto Format :
       {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO,
        BinaryFormat::Raw, BinaryFormat::EVM}) {
    BinaryLoadOptions Options;
    Options.Choice.Format = Format;
    auto Image = loadBinaryBuffer(Buffer, Options);
    ASSERT_FALSE(bool(Image));
    EXPECT_EQ(llvm::toString(Image.takeError()),
              "native buffer does not accept a loader choice");
  }
  auto Image = loadBinaryBuffer(Buffer);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  EXPECT_EQ(Image->Format, BinaryFormat::ELF);
  EXPECT_EQ(Image->Entry, 0x400100U);
  EXPECT_EQ(Image->Raw.size(), Bytes.size());
}

} // namespace
} // namespace neverd
