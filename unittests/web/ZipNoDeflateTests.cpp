//===- ZipNoDeflateTests.cpp - Decoder omission contracts -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Link the ZIP payload owner without zlib support against the ordinary reader.
///
//===----------------------------------------------------------------------===//

#include "BlobStore.h"
#include "ZipFixture.h"
#include "gtest/gtest.h"

#include "neverd/web/PackageArchive.h"

namespace {
using namespace neverd::web;
TEST(WebZipNoDeflate, OmissionKeepsStoredBytesAndRefusesCompressedSelections) {
#ifdef _WIN32
  GTEST_SKIP() << "POSIX blob storage qualification required";
#endif
  if (!packageArchiveAvailable())
    GTEST_SKIP() << "Pinned path policy unavailable";
  ASSERT_FALSE(packageZipDeflateAvailable());
  const test::ZipFixture F({{"stored", "plain"}, {"deflated", "opaque", 8}});
  BlobStore Store(MaxPackageArchiveBytes);
  Store.append(F.Bytes);
  Store.seal();
  Artifact Input;
  Input.ID = "zip-without-decoder";
  Input.Content = Store.whole();
  Input.BlobHash = sha256(F.Bytes);
  const auto A = extractPackageArchive(Input, "zip");
  ASSERT_EQ(A.Members.size(), 2);
  EXPECT_TRUE(A.Members[0].available());
  EXPECT_EQ(A.Members[0].Content.read(0, 5), "plain");
  EXPECT_FALSE(A.Members[1].available());
  EXPECT_EQ(A.Members[1].UnavailableReason, "deflate_unavailable");
  EXPECT_TRUE(A.Members[1].BlobHash.empty());
  EXPECT_EQ(A.Members[1].Content.size(), 0);
  EXPECT_EQ(packageArchiveNamespace(A).Artifacts.size(), 2);
}
} // namespace
