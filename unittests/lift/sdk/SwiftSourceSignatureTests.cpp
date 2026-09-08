#include "../../../lib/sdk/capi/SwiftRuntimeSignatures.h"
#include "../../../lib/sdk/capi/SwiftSourceSignatures.h"
#include "gtest/gtest.h"

using namespace neverd;
using namespace neverd::sdk;

namespace {
llvm::json::Object integerType() {
  return llvm::json::Object{
      {"kind", "integer"}, {"name", "Int64"}, {"bits", 64}, {"signed", true}};
}

llvm::json::Object accessor(bool Setter, bool Struct = false) {
  llvm::json::Array Parameters;
  llvm::json::Array Labels;
  if (Setter) {
    Parameters.push_back(
        llvm::json::Object{{"name", "arg0"}, {"type", integerType()}});
    Labels.push_back("_");
  }
  return llvm::json::Object{
      {"entry", "0x1000"},
      {"mangled_symbol", std::string("$s4Demo3Box") + (Struct ? "V" : "C") +
                             "5values5Int64Vv" + (Setter ? "s" : "g")},
      {"module", "Demo"},
      {"context_kind", Struct ? "struct" : "class"},
      {"context_name", "Box"},
      {"name", "value"},
      {"node_kind", Setter ? "Setter" : "Getter"},
      {"declaration_kind", Setter ? "setter" : "getter"},
      {"is_static", false},
      {"is_mutating", false},
      {"is_mutating_known", !Struct},
      {"parameters", std::move(Parameters)},
      {"labels", std::move(Labels)},
      {"return_type",
       Setter ? llvm::json::Object{{"kind", "void"}, {"name", "Void"}}
              : integerType()}};
}

BinaryImage image(Arch Architecture) {
  BinaryImage Image;
  Image.Format = BinaryFormat::MachO;
  Image.Arch = Architecture;
  Image.Bits = Bitness::Bits64;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 16;
  Text.Data.resize(16);
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Image.Segments.push_back(std::move(Text));
  for (auto Kind : {"C", "V"})
    for (auto Accessor : {"g", "s"})
      Image.addSymbol(std::string("$s4Demo3Box") + Kind + "5values5Int64Vv" +
                          Accessor,
                      0x1000, 16, true);
  return Image;
}

llvm::json::Object runtimeRow(BinaryImage &Image, bool Modify = false) {
  auto Row = accessor(false);
  const std::string Symbol =
      Modify ? "$s4Demo3BoxC5values5Int64VvM" : "$s4Demo3BoxCMa";
  Image.addSymbol(Symbol, 0x1000, 16, true);
  Row["mangled_symbol"] = Symbol;
  Row["declaration_kind"] = "runtime";
  Row["runtime_source_kind"] =
      Modify ? "modify_accessor" : "type_metadata_accessor";
  Row["node_kind"] = Modify ? "ModifyAccessor" : "TypeMetadataAccessFunction";
  Row["name"] = Modify ? "value" : "typeMetadata";
  Row["requires_runtime_source_proof"] = true;
  Row["return_type"] = llvm::json::Object{{"kind", "void"}, {"name", "Void"}};
  if (Modify)
    Row["property_type"] = integerType();
  return Row;
}

SwiftRecoveredType storage() {
  SwiftRecoveredType Type;
  Type.Module = "Demo";
  Type.Name = "Box";
  Type.Kind = "class";
  Type.Status = "recovered";
  Type.Size = 24;
  Type.Alignment = 8;
  Type.Fields.push_back(
      {"value",
       {SwiftSourceType::Kind::Integer, "Int64", 64, true, nullptr},
       16,
       true});
  return Type;
}
} // namespace

TEST(SwiftSourceSignatures, AccessorArgumentsKeepTheirNativeABIPositions) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    const auto Image = image(Architecture);
    for (bool Setter : {false, true}) {
      auto Signature = swift_source::signature(accessor(Setter), Image);
      EXPECT_EQ(Signature.Name, "value");
      EXPECT_EQ(Signature.DeclarationKind, Setter ? "setter" : "getter");
      auto Hint = swift_source::hint(Signature, Architecture);
      ASSERT_EQ(Hint.Parameters.size(), Setter ? 2u : 1u);
      EXPECT_EQ(Hint.Parameters.back().Name, "swift_self");
      EXPECT_EQ(Hint.Parameters.back().Location.RegisterOffset,
                Architecture == Arch::AArch64 ? a64reg::X20 : reg::R13);
      EXPECT_EQ(Hint.ReturnType->Kind,
                Setter ? NdTypeKind::Void : NdTypeKind::Int);
      if (Setter)
        EXPECT_EQ(Hint.Parameters.front().Name, "arg0");
    }
  }
}

TEST(SwiftSourceSignatures, AccessorKindsCannotDisguiseDifferentABIs) {
  auto Image = image(Arch::AArch64);
  for (int Case = 0; Case != 6; ++Case) {
    auto Row = accessor(Case % 2);
    switch (Case) {
    case 0:
      Row["node_kind"] = "Function";
      break;
    case 1:
      Row["is_static"] = true;
      break;
    case 2:
      Row["parameters"] = llvm::json::Array{
          llvm::json::Object{{"name", "arg0"}, {"type", integerType()}}};
      Row["labels"] = llvm::json::Array{"_"};
      break;
    case 3:
      Row["parameters"] = llvm::json::Array{};
      Row["labels"] = llvm::json::Array{};
      break;
    case 4:
      Row["return_type"] =
          llvm::json::Object{{"kind", "void"}, {"name", "Void"}};
      break;
    case 5:
      Row["return_type"] = integerType();
      break;
    }
    EXPECT_THROW(swift_source::signature(Row, Image), std::invalid_argument)
        << Case;
  }
}

TEST(SwiftSourceSignatures, ValueAccessorStillRequiresNativeSelfProof) {
  auto Image = image(Arch::AArch64);
  for (bool Setter : {false, true}) {
    auto Signature = swift_source::signature(accessor(Setter, true), Image);
    EXPECT_FALSE(Signature.ContextLayoutKnown);
    EXPECT_FALSE(Signature.IsMutatingKnown);
    EXPECT_THROW(swift_source::hint(Signature, Arch::AArch64),
                 std::invalid_argument);
  }
}

TEST(SwiftSourceSignatures, AccessorIdentityRequiresAnExecutableNativeSymbol) {
  auto Image = image(Arch::AArch64);
  auto Row = accessor(false);
  Row["entry"] = "0x1008";
  EXPECT_THROW(swift_source::signature(Row, Image), std::invalid_argument);
  Row["entry"] = "0x1000";
  Image.Symbols[0].IsFunc = false;
  EXPECT_THROW(swift_source::signature(Row, Image), std::invalid_argument);
  Image.Symbols[0].IsFunc = true;
  Image.Segments[0].Flags = SegmentFlags::Readable;
  EXPECT_THROW(swift_source::signature(Row, Image), std::invalid_argument);
}

TEST(SwiftRuntimeSignatures,
     RequestPreservesIdentityWithoutInventingRelatedBodies) {
  auto Image = image(Arch::AArch64);
  auto Row = runtimeRow(Image);
  auto Request = swift_source::runtimeRequest(Row, Image);
  EXPECT_EQ(Request.Kind, SwiftRuntimeSourceKind::TypeMetadataAccessor);
  EXPECT_EQ(Request.Signature.Entry, 0x1000u);
  EXPECT_EQ(Request.Signature.MangledSymbol, "$s4Demo3BoxCMa");
  EXPECT_EQ(Request.Signature.DeclarationKind, "runtime");
  EXPECT_FALSE(Request.Initializer);
  EXPECT_FALSE(Request.RelatedEntry);
  EXPECT_FALSE(Request.Signature.ContextLayoutKnown);
}

TEST(SwiftRuntimeSignatures,
     CompilerKindCannotDisguiseAnotherDeclarationOrABI) {
  auto Image = image(Arch::AArch64);
  for (int Case = 0; Case < 5; ++Case) {
    auto Row = runtimeRow(Image);
    switch (Case) {
    case 0:
      Row["runtime_source_kind"] = "destructor";
      break;
    case 1:
      Row["requires_runtime_source_proof"] = false;
      break;
    case 2:
      Row["return_type"] = integerType();
      break;
    case 3:
      Row["name"] = "arbitrary";
      break;
    case 4:
      Row["property_type"] = integerType();
      break;
    }
    EXPECT_THROW(swift_source::runtimeRequest(Row, Image),
                 std::invalid_argument)
        << Case;
  }
}

TEST(SwiftRuntimeSignatures,
     YieldedPropertyRequiresMatchingNativeMutableStorage) {
  auto Image = image(Arch::AArch64);
  auto Row = runtimeRow(Image, true);
  EXPECT_THROW(swift_source::runtimeRequest(Row, Image), std::invalid_argument);
  auto Type = storage();
  auto Request = swift_source::runtimeRequest(Row, Image, {Type});
  EXPECT_EQ(Request.Kind, SwiftRuntimeSourceKind::ModifyAccessor);
  EXPECT_EQ(Request.Signature.Name, "value");
  Type.Fields[0].IsMutable = false;
  EXPECT_THROW(swift_source::runtimeRequest(Row, Image, {Type}),
               std::invalid_argument);
  Type.Fields[0].IsMutable = true;
  Type.Fields[0].Type.Name = "UInt64";
  Type.Fields[0].Type.IsSigned = false;
  EXPECT_THROW(swift_source::runtimeRequest(Row, Image, {Type}),
               std::invalid_argument);
  Row["runtime_source_kind"] = "modify_resume";
  Row["node_kind"] = "CoroutineContinuation";
  Row["continuation_of"] = "ModifyAccessor";
  Row["compiler_suffix"] = ".resume.0";
  EXPECT_THROW(swift_source::runtimeRequest(Row, Image, {storage()}),
               std::invalid_argument);
  const std::string ResumeSymbol = "$s4Demo3BoxC5values5Int64VvM.resume.0";
  Image.addSymbol(ResumeSymbol, 0x1000, 16, true);
  Row["mangled_symbol"] = ResumeSymbol;
  EXPECT_EQ(swift_source::runtimeRequest(Row, Image, {storage()}).Kind,
            SwiftRuntimeSourceKind::ModifyResume);
  Row["compiler_suffix"] = ".other.0";
  EXPECT_THROW(swift_source::runtimeRequest(Row, Image, {storage()}),
               std::invalid_argument);
}

TEST(SwiftRuntimeSignatures,
     AllocatorKeepsInputsButRequiresSeparateNativeProof) {
  auto Image = image(Arch::AArch64);
  auto Row = runtimeRow(Image);
  const std::string Symbol = "$s4Demo3BoxCyACs5Int64VcfC";
  Image.addSymbol(Symbol, 0x1000, 16, true);
  Row["mangled_symbol"] = Symbol;
  Row["name"] = "init";
  Row["node_kind"] = "Allocator";
  Row["runtime_source_kind"] = "allocating_initializer";
  Row["return_type"] = llvm::json::Object{{"kind", "pointer"},
                                          {"name", "UnsafeMutableRawPointer"}};
  Row["parameters"] = llvm::json::Array{
      llvm::json::Object{{"name", "arg0"}, {"type", integerType()}}};
  Row["labels"] = llvm::json::Array{"_"};
  auto Request = swift_source::runtimeRequest(Row, Image, {storage()});
  EXPECT_EQ(Request.Kind, SwiftRuntimeSourceKind::AllocatingInitializer);
  ASSERT_EQ(Request.Signature.Parameters.size(), 1u);
  EXPECT_EQ(Request.Signature.Parameters[0].Type.Name, "Int64");
  EXPECT_FALSE(Request.Initializer);
  Row["return_type"] = llvm::json::Object{{"kind", "void"}, {"name", "Void"}};
  EXPECT_THROW(swift_source::runtimeRequest(Row, Image, {storage()}),
               std::invalid_argument);
}
