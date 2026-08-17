//===- MachOLLVMNameBoundaryTests.cpp - Mach-O LLVM symbol names ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/med/MedABIPass.h"

#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

namespace {

using namespace neverd;

constexpr va_t MainVA = 0x100001000;
constexpr va_t SemanticVA = 0x100001100;
constexpr va_t FreeVA = 0x100002000;
constexpr va_t UnrelatedImportVA = 0x100002100;

MedFunc makeVoidCaller(llvm::StringRef Name, va_t Entry,
                       std::initializer_list<va_t> Targets) {
  MedFunc Func;
  Func.Entry = Entry;
  Func.Name = Name.str();
  Func.ReturnType = NdType::makeVoid();

  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = Entry;
  unsigned Offset = 0;
  for (va_t Target : Targets) {
    MedOp Call;
    Call.Opcode = NdOp::CALL;
    Call.Addr = Entry + Offset;
    Call.addInput(MedVar::makeConst(Target, 8));
    Block.Ops.push_back(std::move(Call));
    Offset += 4;
  }
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = Entry + Offset;
  Block.Ops.push_back(std::move(Return));
  Block.EndAddr = Entry + Offset + 4;
  Func.Blocks.push_back(std::move(Block));
  return Func;
}

MedFunc makeCatchFunction(llvm::StringRef Name, va_t FunctionVA,
                          va_t MayThrowVA, va_t TypeInfoVA,
                          llvm::StringRef TypeName) {
  MedFunc Func;
  Func.Entry = FunctionVA;
  Func.Name = Name.str();
  Func.ReturnType = NdType::makeVoid();

  MedBlock Protected;
  Protected.Id = 0;
  Protected.StartAddr = FunctionVA;
  Protected.EndAddr = FunctionVA + 0x10;
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = FunctionVA + 4;
  Call.addInput(MedVar::makeConst(MayThrowVA, 8));
  Protected.Ops.push_back(std::move(Call));
  MedOp ProtectedReturn;
  ProtectedReturn.Opcode = NdOp::RETURN;
  ProtectedReturn.Addr = FunctionVA + 8;
  Protected.Ops.push_back(std::move(ProtectedReturn));

  MedBlock Handler;
  Handler.Id = 1;
  Handler.StartAddr = FunctionVA + 0x20;
  Handler.EndAddr = FunctionVA + 0x30;
  MedOp HandlerReturn;
  HandlerReturn.Opcode = NdOp::RETURN;
  HandlerReturn.Addr = FunctionVA + 0x24;
  Handler.Ops.push_back(std::move(HandlerReturn));
  Func.Blocks.push_back(std::move(Protected));
  Func.Blocks.push_back(std::move(Handler));

  ExceptionFunction EH;
  EH.CodeRange = {FunctionVA, FunctionVA + 0x30};
  EH.Encoding = ExceptionEncoding::DwarfFDE;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::GxxPersonalityV0;
  EH.Itanium.emplace();
  EH.Itanium->IsCallSiteAddressForm = true;

  ItaniumCallSite Site;
  Site.GuardedRange = {FunctionVA, FunctionVA + 0x10};
  Site.LandingPadVA = FunctionVA + 0x20;
  Site.FirstActionOffset = 1;
  EH.Itanium->CallSites.push_back(Site);
  EH.Itanium->Actions.push_back({1, 1, std::nullopt});

  ItaniumTypeEntry Type;
  Type.Index = 1;
  Type.TypeInfoVA = TypeInfoVA;
  Type.TypeName = TypeName.str();
  EH.Itanium->TypeTable.push_back(std::move(Type));
  Func.ExceptionMetadata = std::move(EH);
  return Func;
}

void expectValidModule(const llvm::Module &Module) {
  std::string Verification;
  llvm::raw_string_ostream OS(Verification);
  EXPECT_FALSE(llvm::verifyModule(Module, &OS)) << OS.str();
}

TEST(MachOLLVMNameBoundary, CanonicalizesProvenFunctionAndImportObjectNames) {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Format = BinaryFormat::MachO;
  Image.Bits = Bitness::Bits64;

  Symbol Main = Symbol::makeFunc(MainVA, 16);
  Main.Name = "_main";
  Image.Symbols.push_back(std::move(Main));

  Import Free;
  Free.Name = "_free";
  Free.IATAddr = FreeVA;
  Image.Imports.push_back(std::move(Free));

  // A raw object name elsewhere is not evidence that an already-canonical
  // debug/source name at another address carries a Darwin object prefix.
  Import Unrelated;
  Unrelated.Name = "_semantic_name";
  Unrelated.IATAddr = UnrelatedImportVA;
  Image.Imports.push_back(std::move(Unrelated));

  MedFunc MainFunc = makeVoidCaller("_main", MainVA, {FreeVA});
  MedFunc SemanticFunc = makeVoidCaller("_semantic_name", SemanticVA, {});

  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(
      {MainFunc, SemanticFunc}, Context, "macho-name-boundary", Arch::AArch64,
      {{FreeVA, "_free"}, {UnrelatedImportVA, "_semantic_name"}}, &Image,
      BinaryFormat::MachO);
  ASSERT_NE(Module, nullptr);
  expectValidModule(*Module);

  EXPECT_NE(Module->getFunction("main"), nullptr);
  EXPECT_EQ(Module->getFunction("_main"), nullptr);
  EXPECT_NE(Module->getFunction("free"), nullptr);
  EXPECT_EQ(Module->getFunction("_free"), nullptr);

  llvm::Function *Semantic = Module->getFunction("_semantic_name");
  ASSERT_NE(Semantic, nullptr);
  EXPECT_FALSE(Semantic->isDeclaration());
  EXPECT_EQ(Module->getFunction("semantic_name"), nullptr);
}

TEST(MachOLLVMNameBoundary, LeavesNonMachOObjectNamesUnchanged) {
  MedFunc Caller = makeVoidCaller("elf_entry", MainVA, {FreeVA});

  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit({Caller}, Context, "elf-name-boundary",
                                      Arch::AArch64, {{FreeVA, "_free"}},
                                      nullptr, BinaryFormat::ELF);
  ASSERT_NE(Module, nullptr);
  expectValidModule(*Module);
  EXPECT_NE(Module->getFunction("_free"), nullptr);
  EXPECT_EQ(Module->getFunction("free"), nullptr);
}

TEST(MachOLLVMNameBoundary, CanonicalizesRelocationCalleeAtTheProvenCallSite) {
  constexpr va_t CallerVA = 0x100003000;
  constexpr va_t OtherTargetVA = 0x100003100;

  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Format = BinaryFormat::MachO;
  Image.Bits = Bitness::Bits64;

  RelocationEntry AssertRelocation;
  AssertRelocation.Address = CallerVA;
  AssertRelocation.SymbolName = "___assert_rtn";
  Image.Relocations.push_back(std::move(AssertRelocation));

  // The same spelling at a different address is deliberately unrelated to
  // OtherTargetVA and must not make that call lose a semantic underscore.
  Import Unrelated;
  Unrelated.Name = "_semantic_name";
  Unrelated.IATAddr = UnrelatedImportVA;
  Image.Imports.push_back(std::move(Unrelated));

  MedFunc Caller =
      makeVoidCaller("relocation_caller", CallerVA, {0, OtherTargetVA});
  const std::map<va_t, std::string> Names{{OtherTargetVA, "_semantic_name"}};
  recoverCallAbi(Caller, Arch::AArch64, Names, &Image);

  ASSERT_EQ(Caller.CallInfos.size(), 2u);
  EXPECT_EQ(Caller.CallInfos[0].TargetName, "___assert_rtn");
  EXPECT_EQ(Caller.CallInfos[1].TargetName, "_semantic_name");

  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(
      {Caller}, Context, "macho-relocation-name-boundary", Arch::AArch64,
      {{UnrelatedImportVA, "_semantic_name"}}, &Image, BinaryFormat::MachO);
  ASSERT_NE(Module, nullptr);
  expectValidModule(*Module);

  EXPECT_NE(Module->getFunction("__assert_rtn"), nullptr);
  EXPECT_EQ(Module->getFunction("___assert_rtn"), nullptr);
  EXPECT_NE(Module->getFunction("_semantic_name"), nullptr);
  EXPECT_EQ(Module->getFunction("semantic_name"), nullptr);
}

TEST(MachOLLVMNameBoundary, CanonicalizesOnlyProvenMachOTypeInfoObjectNames) {
  constexpr va_t RawFunctionVA = 0x100004000;
  constexpr va_t RawThrowVA = 0x100004100;
  constexpr va_t RawTypeInfoVA = 0x100004200;
  constexpr va_t DecodedFunctionVA = 0x100005000;
  constexpr va_t DecodedThrowVA = 0x100005100;
  constexpr va_t DecodedTypeInfoVA = 0x100005200;
  constexpr llvm::StringLiteral RawTypeName("__ZTI18RawMachOCatchType");
  constexpr llvm::StringLiteral CanonicalRawTypeName("_ZTI18RawMachOCatchType");
  constexpr llvm::StringLiteral DecodedTypeName("_ZTI19DecodedCatchType");

  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Format = BinaryFormat::MachO;
  Image.Bits = Bitness::Bits64;
  Symbol RawType;
  RawType.Name = RawTypeName.str();
  RawType.Addr = RawTypeInfoVA;
  Image.Symbols.push_back(std::move(RawType));

  // A default/absent TypeInfoSlotVA is zero.  An unrelated relocation at
  // address zero must not turn a decoder-provided canonical name into an
  // object-file spelling.
  RelocationEntry ZeroAddressCollision;
  ZeroAddressCollision.SymbolName = DecodedTypeName.str();
  Image.Relocations.push_back(std::move(ZeroAddressCollision));

  MedFunc Raw = makeCatchFunction("raw_typeinfo_caller", RawFunctionVA,
                                  RawThrowVA, RawTypeInfoVA, RawTypeName);
  MedFunc Decoded =
      makeCatchFunction("decoded_typeinfo_caller", DecodedFunctionVA,
                        DecodedThrowVA, DecodedTypeInfoVA, DecodedTypeName);

  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit(
      {Raw, Decoded}, Context, "macho-rtti-name-boundary", Arch::AArch64,
      {{RawThrowVA, "may_throw"}, {DecodedThrowVA, "may_throw_again"}}, &Image,
      BinaryFormat::MachO);
  ASSERT_NE(Module, nullptr);
  expectValidModule(*Module);

  EXPECT_NE(Module->getNamedGlobal(CanonicalRawTypeName), nullptr);
  EXPECT_EQ(Module->getNamedGlobal(RawTypeName), nullptr);
  EXPECT_NE(Module->getNamedGlobal(DecodedTypeName), nullptr);
  EXPECT_EQ(Module->getNamedGlobal("ZTI19DecodedCatchType"), nullptr);
}

} // namespace
