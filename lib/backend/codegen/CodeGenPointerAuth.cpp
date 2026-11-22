//===- CodeGenPointerAuth.cpp - Import pointer authentication globals -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "CodeGenDetail.h"
#include "MachO/MachOArm64eABI.h"

#include "neverd/backend/llvm/AArch64/LLVMAArch64ReturnProtection.h"
#include "neverd/ir/intrinsics/AArch64/AArch64PAuth.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/WithColor.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Utils/LowerGlobalDtors.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

#include <climits>
#include <cstdint>
#include <iterator>

namespace neverd {
namespace {
#define CODEGEN_PAUTH_TEXT(Name, Text) constexpr char Name[] = Text;
#define CODEGEN_PAUTH_FIELD(Name, Index) constexpr unsigned Name = Index;
#include "CodeGenPointerAuth.def"
#define CODEGEN_RUNTIME_GLOBAL(Name, Text)                                     \
  constexpr llvm::StringLiteral Name = Text;
#include "CodeGenRuntimeGlobals.def"

// A Mach-O authenticated relocation blends the address of its own slot. A
// constant used in code can instead materialize an explicit discriminator.
bool usesSigningAddress(const llvm::GlobalVariable &Descriptor,
                        const llvm::ConstantPtrAuth &Recipe,
                        const llvm::DataLayout &DL) {
  const auto Bits = DL.getIndexSizeInBits(Descriptor.getAddressSpace());
  llvm::APInt Offset(Bits, 0);
  const auto *Base =
      Recipe.getAddrDiscriminator()->stripAndAccumulateConstantOffsets(
          DL, Offset, true);
  if (const auto *G = llvm::dyn_cast<llvm::GlobalVariable>(Base))
    if (G->getSection() == kLegacyApplePAuthSection)
      return false;
  using Slot = std::pair<const llvm::Constant *, uint64_t>;
  llvm::SmallVector<Slot> Pending{{&Descriptor, 0}};
  llvm::DenseSet<Slot> Seen;
  while (!Pending.empty()) {
    const auto [Value, At] = Pending.pop_back_val();
    if (!Seen.insert({Value, At}).second)
      continue;
    for (const auto &Use : Value->uses()) {
      const auto *User = Use.getUser();
      if (llvm::isa<llvm::Instruction, llvm::Function>(User))
        continue;
      if (const auto *G = llvm::dyn_cast<llvm::GlobalVariable>(User)) {
        const bool Structors =
            G->getName() == kGlobalCtors || G->getName() == kGlobalDtors;
        if (Structors) {
          if (!Recipe.hasSpecialAddressDiscriminator(
                  llvm::ConstantPtrAuth::AddrDiscriminator_CtorsDtors))
            return false;
        } else if (G != Base || Offset != At) {
          return false;
        }
        continue;
      }
      const auto *C = llvm::dyn_cast<llvm::Constant>(User);
      if (!C)
        return false;
      const unsigned I = Use.getOperandNo();
      if (const auto *S = llvm::dyn_cast<llvm::ConstantStruct>(C))
        Pending.emplace_back(
            C, At + DL.getStructLayout(S->getType())->getElementOffset(I));
      else if (llvm::isa<llvm::ConstantArray, llvm::ConstantVector>(C))
        Pending.emplace_back(
            C, At + I * DL.getTypeAllocSize(Value->getType()).getFixedValue());
      else if (const auto *CE = llvm::dyn_cast<llvm::ConstantExpr>(C);
               CE && CE->getOpcode() == llvm::Instruction::PtrToInt &&
               CE->getType()->isIntegerTy(Bits))
        Pending.emplace_back(C, At);
      else
        return false;
    }
  }
  return true;
}

llvm::Constant *importAppleDescriptor(llvm::GlobalVariable &G,
                                      const llvm::DataLayout &DL) {
  auto &C = G.getContext();
  auto *Ptr = llvm::PointerType::getUnqual(C);
  auto *I32 = llvm::Type::getInt32Ty(C);
  auto *I64 = llvm::Type::getInt64Ty(C);
  const auto *Schema = llvm::dyn_cast<llvm::StructType>(G.getValueType());
  if (!Schema ||
      !Schema->isLayoutIdentical(
          llvm::StructType::get(C, {Ptr, I32, I64, I64})) ||
      !G.hasInitializer() || !G.isConstant() || G.getType() != Ptr ||
      G.isThreadLocal())
    return nullptr;
  auto *Init = G.getInitializer();
  auto *Target = Init->getAggregateElement(kLegacyTarget);
  auto *Key = llvm::dyn_cast_or_null<llvm::ConstantInt>(
      Init->getAggregateElement(kLegacyKey));
  auto *Addr = Init->getAggregateElement(kLegacyAddress);
  auto *Disc = llvm::dyn_cast_or_null<llvm::ConstantInt>(
      Init->getAggregateElement(kLegacyDiscriminator));
  if (!Target || !Key || Key->getZExtValue() > kAArch64PAuthKeyDB || !Addr ||
      !Disc || !Disc->getValue().isIntN(sizeof(uint16_t) * CHAR_BIT))
    return nullptr;
  llvm::APInt Addend(DL.getIndexSizeInBits(G.getAddressSpace()), 0);
  const auto *Base =
      Target->stripAndAccumulateConstantOffsets(DL, Addend, true);
  if (!llvm::isa<llvm::GlobalValue>(Base) ||
      (llvm::isa<llvm::GlobalVariable>(Base) &&
       llvm::cast<llvm::GlobalVariable>(Base)->getSection() ==
           kLegacyApplePAuthSection))
    return nullptr;
  auto *Null = llvm::ConstantPointerNull::get(Ptr);
  llvm::Constant *Storage = Null;
  if (!Addr->isNullValue()) {
    if (auto *Cast = llvm::dyn_cast<llvm::PtrToIntOperator>(Addr))
      Storage = llvm::cast<llvm::Constant>(Cast->getPointerOperand());
    else if (llvm::isa<llvm::ConstantInt>(Addr))
      Storage = llvm::ConstantExpr::getIntToPtr(Addr, Ptr);
    else
      return nullptr;
    if (Storage->getType() != Ptr)
      return nullptr;
  }
  auto *Recipe = llvm::ConstantPtrAuth::get(Target, Key, Disc, Storage, Null);
  if (Recipe->hasAddressDiscriminator() && !usesSigningAddress(G, *Recipe, DL))
    return nullptr;
  return Recipe;
}

// dyld ignores legacy termination sections in arm64e images. LLVM's generic
// lowering preserves priority and associated symbols. Its callback crosses the
// Darwin function-pointer ABI when the image registers it with libSystem.
bool lowerDarwinDestructors(llvm::Module &M) {
  auto *Dtors = M.getGlobalVariable(kGlobalDtors);
  if (!Dtors || !Dtors->hasInitializer() ||
      Dtors->getInitializer()->isNullValue())
    return true;
  const auto *Entries =
      llvm::dyn_cast<llvm::ConstantArray>(Dtors->getInitializer());
  if (!Entries)
    return false;
  llvm::IRBuilder<> B(M.getContext());
  const auto *DtorType = llvm::FunctionType::get(B.getVoidTy(), false);
  llvm::SmallVector<llvm::Constant *> Rows;
  for (const auto &Entry : Entries->operands()) {
    auto *Row = llvm::cast<llvm::Constant>(Entry.get());
    auto *Target = Row->getAggregateElement(kStructorFunction);
    if (Target && Target->isNullValue())
      break;
    const auto *Priority = llvm::dyn_cast_or_null<llvm::ConstantInt>(
        Row->getAggregateElement(kStructorPriority));
    const auto *Association = Row->getAggregateElement(kStructorAssociation);
    if (!Target || !Priority ||
        !Priority->getValue().isIntN(sizeof(uint16_t) * CHAR_BIT) ||
        !Association ||
        (!Association->isNullValue() &&
         !llvm::isa<llvm::GlobalValue>(Association->stripPointerCasts())))
      return false;
    if (const auto *Auth = llvm::dyn_cast<llvm::ConstantPtrAuth>(Target)) {
      if (Auth->getKey()->getZExtValue() != kAArch64PAuthKeyIA ||
          !Auth->getDiscriminator()->isZero() ||
          Auth->hasAddressDiscriminator() ||
          !Auth->getDeactivationSymbol()->isNullValue())
        return false;
      // The initializer statically names the function that the wrapper calls.
      Target = Auth->getPointer();
      Row = llvm::ConstantStruct::get(
          llvm::cast<llvm::StructType>(Row->getType()),
          {Row->getAggregateElement(kStructorPriority), Target,
           Row->getAggregateElement(kStructorAssociation)});
    }
    const auto *F =
        llvm::dyn_cast<llvm::Function>(Target->stripPointerCastsAndAliases());
    if (!F || F->getFunctionType() != DtorType ||
        F->getCallingConv() != llvm::CallingConv::C)
      return false;
    Rows.push_back(Row);
  }
  if (Rows.empty()) {
    Dtors->eraseFromParent();
    return true;
  }
  auto *Ptr = B.getPtrTy();
  if (const auto *Value = M.getNamedValue(macho_arm64e_abi::kCxaAtExit)) {
    const auto *F = llvm::dyn_cast<llvm::Function>(Value);
    if (!F ||
        F->getFunctionType() !=
            llvm::FunctionType::get(B.getInt32Ty(), {Ptr, Ptr, Ptr}, false) ||
        F->getCallingConv() != llvm::CallingConv::C)
      return false;
  }
  const auto *Handle = M.getNamedValue(macho_arm64e_abi::kDSOHandle);
  if (Handle &&
      (!llvm::isa<llvm::GlobalVariable>(Handle) || Handle->getAddressSpace()))
    return false;
  // Remove the null terminator and its tail as well as normalizing signed
  // entries. The generic pass otherwise skips a zeroinitializer row.
  size_t Index = 0;
  llvm::transformGlobalDtors(M, [&](llvm::Constant *) -> llvm::Constant * {
    return Index < Rows.size() ? Rows[Index++] : nullptr;
  });
  auto Last = std::prev(M.end());
  llvm::ModuleAnalysisManager AM;
  llvm::LowerGlobalDtorsPass().run(M, AM);
  if (!Handle)
    if (auto *G = M.getGlobalVariable(macho_arm64e_abi::kDSOHandle))
      G->setLinkage(llvm::GlobalValue::ExternalLinkage);
  auto *Register = M.getFunction(macho_arm64e_abi::kCxaAtExit);
  auto *Null = llvm::ConstantPointerNull::get(Ptr);
  for (auto &F : llvm::make_range(std::next(Last), M.end())) {
    if (F.isDeclaration())
      continue;
    // Keep a local function symbol for the native initializer relocation.
    F.setLinkage(llvm::GlobalValue::InternalLinkage);
    F.addFnAttr(kReturnSigningArm64eAttribute);
    F.setUWTableKind(llvm::UWTableKind::Sync);
    for (auto &I : llvm::instructions(F)) {
      auto *Call = llvm::dyn_cast<llvm::CallInst>(&I);
      if (Call && Call->getCalledFunction() == Register) {
        auto *Callback = llvm::cast<llvm::Function>(Call->getArgOperand(0));
        Call->setArgOperand(0, llvm::ConstantPtrAuth::get(
                                   Callback, B.getInt32(kAArch64PAuthKeyIA),
                                   B.getInt64(0), Null, Null));
      }
    }
  }
  return !M.getGlobalVariable(kGlobalDtors);
}

bool prepareAuthenticatedCalls(llvm::Module &Module) {
#if !__has_include("llvm/BinaryFormat/AArch64AuthenticatedCalls.def")
  llvm::SmallVector<llvm::CallBase *> Calls;
#endif
  for (auto &F : Module)
    for (auto &I : llvm::instructions(F)) {
      auto *Call = llvm::dyn_cast<llvm::CallBase>(&I);
      if (!Call || !Call->hasOperandBundles())
        continue;
      auto Bundle = Call->getOperandBundle(llvm::LLVMContext::OB_ptrauth);
      if (!Bundle)
        continue;
      // LLVM IR leaves the allowed key range to the target.
      const auto *Key =
          llvm::dyn_cast<llvm::ConstantInt>(Bundle->Inputs[kCallKey].get());
      if (!Key || Key->getValue().ugt(kAArch64PAuthKeyDB)) {
        llvm::WithColor::error() << kInvalidCallKey << F.getName() << '\n';
        return false;
      }
#if !__has_include("llvm/BinaryFormat/AArch64AuthenticatedCalls.def")
      const bool PAuthLR = F.hasFnAttribute(kReturnSigningPCAttribute);
      if (PAuthLR && Call->isMustTailCall()) {
        llvm::WithColor::error()
            << kUnsupportedPAuthLRMustTail << F.getName() << '\n';
        return false;
      }
      if (PAuthLR || Key->getZExtValue() >= kAArch64PAuthKeyDA)
        Calls.push_back(Call);
#endif
    }
#if !__has_include("llvm/BinaryFormat/AArch64AuthenticatedCalls.def")
  for (auto *Call : Calls) {
    // Older backends cannot keep x16 available for return authentication
    // across an authenticated tail call.
    if (auto *CI = llvm::dyn_cast<llvm::CallInst>(Call);
        CI && CI->getFunction()->hasFnAttribute(kReturnSigningPCAttribute))
      CI->setTailCallKind(llvm::CallInst::TCK_NoTail);
    auto Bundle = *Call->getOperandBundle(llvm::LLVMContext::OB_ptrauth);
    const auto &Key = Bundle.Inputs[kCallKey];
    const auto &Disc = Bundle.Inputs[kCallDiscriminator];
    if (llvm::cast<llvm::ConstantInt>(Key.get())->getZExtValue() <
        kAArch64PAuthKeyDA)
      continue;
    // Verify the original field schema before producing an IA-signed target
    // that this backend can consume. Keep all call arguments and unwind edges.
    llvm::IRBuilder<> B(Call);
    auto *Target = Call->getCalledOperand();
    auto *IA = B.getInt32(kAArch64PAuthKeyIA);
    auto *Zero = B.getInt64(0);
    auto *Signed = B.CreateIntrinsic(llvm::Intrinsic::ptrauth_resign, {},
                                     {B.CreatePtrToInt(Target, B.getInt64Ty()),
                                      Key.get(), Disc.get(), IA, Zero});
    Call->setCalledOperand(B.CreateIntToPtr(Signed, Target->getType()));
    Call->setOperand(Key.getOperandNo(), IA);
    Call->setOperand(Disc.getOperandNo(), Zero);
  }
#endif
  return true;
}

#if !__has_include("llvm/MC/MCPointerAuthInfo.h")
// The prebuilt compiler can emit native object authentication relocations,
// but its final-address callback cannot retain their signing schema. Detect
// nested constants as well as top-level pointer initializers before codegen.
bool hasRecipeCallbacksFor(const llvm::Module &Module) {
  llvm::SmallVector<const llvm::Constant *> Pending;
  llvm::DenseSet<const llvm::Constant *> Seen;
  for (const auto &Global : Module.globals())
    if (Global.hasInitializer() &&
        Global.getSection() != kLegacyApplePAuthSection)
      Pending.push_back(Global.getInitializer());
  while (!Pending.empty()) {
    const auto *Value = Pending.pop_back_val();
    if (!Seen.insert(Value).second)
      continue;
    const auto *G = llvm::dyn_cast<llvm::GlobalVariable>(Value);
    if (llvm::isa<llvm::ConstantPtrAuth>(Value) ||
        (G && G->getSection() == kLegacyApplePAuthSection)) {
      llvm::WithColor::error() << kPAuthRecipeCallbackMissing;
      return false;
    }
    if (llvm::isa<llvm::GlobalValue>(Value))
      continue;
    for (const auto &Operand : Value->operands())
      if (const auto *Nested = llvm::dyn_cast<llvm::Constant>(Operand))
        Pending.push_back(Nested);
  }
  return true;
}
#endif
} // namespace

bool preparePointerAuthentication(llvm::Module &Module,
                                  const llvm::Triple &Triple,
                                  bool RequireRecipes) {
  llvm::SmallVector<std::pair<llvm::GlobalVariable *, llvm::Constant *>>
      Imports;
  for (auto &Global : Module.globals()) {
    if (Global.getSection() != kLegacyApplePAuthSection)
      continue;
    auto *Value = Triple.isArm64e() && Triple.isOSBinFormatMachO()
                      ? importAppleDescriptor(Global, Module.getDataLayout())
                      : nullptr;
    if (!Value) {
      llvm::WithColor::error()
          << kLegacyApplePAuthInvalid << Global.getName() << '\n';
      return false;
    }
    Imports.emplace_back(&Global, Value);
  }
#if !__has_include("llvm/MC/MCPointerAuthInfo.h")
  if (RequireRecipes && !hasRecipeCallbacksFor(Module))
    return false;
#endif
  if (Triple.isAArch64() && !prepareAuthenticatedCalls(Module))
    return false;
  // Validate the entire module before replacing references, including those
  // nested in aggregate initializers. The descriptor itself has no storage.
  for (auto [Global, Value] : Imports) {
    Global->replaceAllUsesWith(Value);
    Global->eraseFromParent();
  }
  if (Triple.isArm64e() && Triple.isOSBinFormatMachO() &&
      !lowerDarwinDestructors(Module)) {
    llvm::WithColor::error() << kInvalidDarwinDtors;
    return false;
  }
  return true;
}
} // namespace neverd
