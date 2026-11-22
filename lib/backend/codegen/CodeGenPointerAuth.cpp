//===- CodeGenPointerAuth.cpp - Import pointer authentication globals -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "CodeGenDetail.h"

#include "neverd/ir/intrinsics/AArch64/AArch64PAuth.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/WithColor.h"
#include "llvm/TargetParser/Triple.h"

#include <climits>
#include <cstdint>

namespace neverd {
namespace {
#define CODEGEN_PAUTH_TEXT(Name, Text) constexpr char Name[] = Text;
#define CODEGEN_PAUTH_FIELD(Name, Index) constexpr unsigned Name = Index;
#include "CodeGenPointerAuth.def"

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

bool preparePointerAuthenticationGlobals(llvm::Module &Module,
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
  // Validate the entire module before replacing references, including those
  // nested in aggregate initializers. The descriptor itself has no storage.
  for (auto [Global, Value] : Imports) {
    Global->replaceAllUsesWith(Value);
    Global->eraseFromParent();
  }
  return true;
}
} // namespace neverd
