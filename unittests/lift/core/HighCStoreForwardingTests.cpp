//===- HighCStoreForwardingTests.cpp - Bounded forwarding tests ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/c/HighC/HighCWriter.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/pass/HighC/HighCPasses.h"
#include "neverd/backend/c/render/CTypeFormat.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <cctype>
#include <cstddef>
#include <optional>
#include <string>

namespace {

using namespace neverd;

TEST(HighCStoreForwarding, IncompleteFrameRecordsKeepTheirInteriorFields) {
  class LazyFrameTypes : public NullDebugContext {
  public:
    void completeType(const TypeRef &Type) const override {
      if (!Type || Type->Kind != NdTypeKind::Struct || Type->IsEnum ||
          Type->SourceName != "Pair" || !Type->FieldDisplayNames.empty())
        return;
      Type->FieldDisplayNames = {"first", "second"};
      Type->FieldDisplayOffsets = {0, 4};
      Type->FieldDisplayTypes = {NdType::makeInt(4), NdType::makeInt(4)};
    }
  } Debug;
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  CEmitterOptions Options;
  HighCWriter Writer(OS, Options, &Debug);
  for (const bool KnownExtent : {false, true})
    for (const bool CallOverlay : {false, true}) {
      SCOPED_TRACE(KnownExtent);
      SCOPED_TRACE(CallOverlay);
      Writer.FrameSlots.clear();
      const TypeRef Record =
          NdType::makeNamedRecord("Pair", KnownExtent ? 8 : 0);
      HighCWriter::NamedFrameSlot Slot;
      Slot.Name = "pair";
      Slot.Type = CallOverlay ? NdType::makeInt(8) : Record;
      Slot.CallType = CallOverlay ? Record : nullptr;
      Writer.FrameSlots[-16] = Slot;
      Writer.FrameSlots[-24].Name = "integer";
      Writer.FrameSlots[-24].Type = NdType::makeInt(8);
      EXPECT_EQ(Writer.frameTypedMemberAccess(-12, 4),
                std::optional<std::string>("pair.second"));
      const TypeRef Member = Writer.frameTypedMemberType(-12, 4);
      ASSERT_TRUE(Member);
      EXPECT_EQ(Member->Kind, NdTypeKind::Int);
      EXPECT_EQ(Member->Size, 4);
      EXPECT_FALSE(Writer.frameTypedMemberAccess(-16, 4));
      EXPECT_FALSE(Writer.frameTypedMemberType(-16, 4));
      EXPECT_FALSE(Writer.frameTypedMemberAccess(-12, 8));
      EXPECT_FALSE(Writer.frameTypedMemberType(-12, 8));

      // Two records covering the same bytes cannot choose one member's name.
      const TypeRef Overlap = NdType::makeNamedRecord("Overlap", 12);
      Overlap->FieldDisplayNames = {"value"};
      Overlap->FieldDisplayOffsets = {8};
      Overlap->FieldDisplayTypes = {NdType::makeInt(4)};
      Writer.FrameSlots[-20].Name = "overlap";
      Writer.FrameSlots[-20].Type = Overlap;
      EXPECT_FALSE(Writer.frameTypedMemberAccess(-12, 4));
      EXPECT_FALSE(Writer.frameTypedMemberType(-12, 4));
    }
}

TEST(HighCStoreForwarding, FrameMemberQueriesFollowNewSlotsAfterProjection) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  HighCWriter Writer(OS, Options, nullptr);
  HighFunc Func;
  Func.Entry = 0x1000;
  Func.Name = "scalar_frame";
  Func.ReturnType = NdType::makeInt(4);
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = HighExpr::makeConst(7, 4);
  Func.Body = {Return};

  for (const bool CallOverlay : {false, true}) {
    SCOPED_TRACE(CallOverlay);
    // A completed projection cannot constrain subsequent queries about new
    // frame records or call overlays installed on the same writer.
    Writer.writeFunctionProjection(Func);
    const TypeRef Record = NdType::makeNamedRecord("Pair", 8);
    Record->FieldDisplayNames = {"first", "second"};
    Record->FieldDisplayOffsets = {0, 4};
    Record->FieldDisplayTypes = {NdType::makeInt(4), NdType::makeInt(4)};
    auto &Slot = Writer.FrameSlots[-16];
    Slot.Name = "next_pair";
    Slot.Type = CallOverlay ? NdType::makeInt(8) : Record;
    Slot.CallType = CallOverlay ? Record : nullptr;
    EXPECT_EQ(Writer.frameTypedMemberAccess(-12, 4),
              std::optional<std::string>("next_pair.second"));
    const TypeRef Member = Writer.frameTypedMemberType(-12, 4);
    ASSERT_TRUE(Member);
    EXPECT_EQ(Member->Kind, NdTypeKind::Int);
    EXPECT_EQ(Member->Size, 4);
  }
}

ExprPtr makeParam(unsigned Id, uint16_t Size, TypeRef Type,
                  Arch Architecture = Arch::X64) {
  MedVar Param;
  Param.Kind = MedVar::Param;
  Param.Id = static_cast<int>(Id);
  Param.Size = Size;
  Param.TheArch = Architecture;
  return HighExpr::makeVar(Param, Type);
}

ExprPtr frameSlot(unsigned Offset, Arch Architecture = Arch::X64) {
  MedVar SP;
  SP.Kind = MedVar::Reg;
  SP.Id = 100;
  SP.Size = Architecture == Arch::X86 || Architecture == Arch::ARM ? 4 : 8;
  SP.TheArch = Architecture;
  SP.RegOff = getTargetRegInfo(Architecture).StackPointer;
  return HighExpr::makeBinop(NdOp::INT_SUB, HighExpr::makeVar(SP),
                             HighExpr::makeConst(Offset, SP.Size));
}

std::optional<llvm::StringRef> functionBody(llvm::StringRef Output,
                                            llvm::StringRef Name) {
  size_t NamePos = Output.find(Name.str() + "(");
  if (NamePos == llvm::StringRef::npos)
    return std::nullopt;
  size_t OpenBrace = Output.find('{', NamePos);
  if (OpenBrace == llvm::StringRef::npos)
    return std::nullopt;

  unsigned Depth = 0;
  for (size_t I = OpenBrace; I < Output.size(); ++I) {
    if (Output[I] == '{')
      ++Depth;
    else if (Output[I] == '}' && --Depth == 0)
      return Output.slice(OpenBrace + 1, I);
  }
  return std::nullopt;
}

size_t countOccurrences(llvm::StringRef Text, llvm::StringRef Needle) {
  size_t Count = 0;
  size_t From = 0;
  while ((From = Text.find(Needle, From)) != llvm::StringRef::npos) {
    ++Count;
    From += Needle.size();
  }
  return Count;
}

size_t countPointerDerefStores(llvm::StringRef Text) {
  size_t Count = countOccurrences(Text, "neverd_mem_store_") +
                 countOccurrences(Text, "__builtin_memcpy((void *)");
  size_t From = 0;
  while ((From = Text.find("*(", From)) != llvm::StringRef::npos) {
    const size_t Eq = Text.find('=', From);
    const size_t Semi = Text.find(';', From);
    if (Eq != llvm::StringRef::npos && Semi != llvm::StringRef::npos &&
        Eq < Semi)
      ++Count;
    From += 2;
  }
  From = 0;
  while ((From = Text.find("var_", From)) != llvm::StringRef::npos) {
    const size_t Eq = Text.find('=', From);
    const size_t Semi = Text.find(';', From);
    if (Eq != llvm::StringRef::npos && Semi != llvm::StringRef::npos &&
        Eq < Semi && Eq - From < 20)
      ++Count;
    From += 4;
  }
  return Count;
}

size_t countPointerDerefLoads(llvm::StringRef Text) {
  size_t Count = countOccurrences(Text, "neverd_mem_load_") +
                 countOccurrences(Text, "__builtin_memcpy(&");
  size_t From = 0;
  while ((From = Text.find("*(", From)) != llvm::StringRef::npos) {
    const size_t Eq = Text.find('=', From);
    const size_t Semi = Text.find(';', From);
    if (Semi != llvm::StringRef::npos &&
        (Eq == llvm::StringRef::npos || Eq > Semi))
      ++Count;
    From += 2;
  }
  return Count;
}

TEST(HighCStoreForwarding, BoundsRepeatedTransitiveExpansion) {
  constexpr unsigned ChainLength = 18;
  auto I32 = NdType::makeInt(4);
  auto I32Ptr = NdType::makePtr(I32);

  HighFunc Func;
  Func.Name = "bounded_store_forwarding";
  Func.FrameSize = ChainLength * 4;
  Func.ReturnType = I32;
  Func.Params.push_back({"arg0", I32});

  auto Seed = [&] { return makeParam(0, 4, I32); };
  auto Slot = [&](unsigned Index) { return frameSlot((Index + 1) * 4); };

  for (unsigned I = 0; I < ChainLength; ++I) {
    HighStmt Store;
    Store.Kind = StmtKind::Store;
    Store.StoreAddr = Slot(I);
    if (I == 0) {
      Store.StoreVal = Seed();
    } else {
      auto Previous = [&] { return HighExpr::makeLoad(Slot(I - 1), I32); };
      Store.StoreVal =
          HighExpr::makeBinop(NdOp::INT_ADD, Previous(), Previous());
    }
    Func.Body.push_back(std::move(Store));
  }

  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = HighExpr::makeLoad(Slot(ChainLength - 1), I32);
  Func.Body.push_back(std::move(Return));

  std::string Output;
  llvm::raw_string_ostream OS(Output);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  ASSERT_TRUE(HighCEmitter().emit({Func}, OS, Options));
  OS.flush();

  auto Body = functionBody(Output, Func.Name);
  ASSERT_TRUE(Body.has_value());
  EXPECT_LT(Body->size(), 32u * 1024u) << Body->take_front(4096).str();

  // Crossing the inline budget must keep a real memory boundary.  Keeping
  // every store would avoid the blow-up but regress ordinary forwarding, so
  // require both retained and eliminated stores in this same chain.
  const size_t StoreCalls = countPointerDerefStores(*Body);
  EXPECT_GT(StoreCalls, 0u) << Body->take_front(4096).str();
  EXPECT_LT(StoreCalls, ChainLength) << Body->take_front(4096).str();
  EXPECT_TRUE(countPointerDerefLoads(*Body) > 0 || Body->contains("var_"))
      << Body->take_front(4096).str();
  EXPECT_TRUE(Body->contains("return ")) << Body->take_front(4096).str();
  EXPECT_TRUE(Body->contains("arg0")) << Body->take_front(4096).str();
  EXPECT_FALSE(Body->contains("truncated: expr too deep"))
      << Body->take_front(4096).str();
}

// \p Portable prints memory accesses as byte copies instead of the default
// aligned(1), may_alias pointers, for tests that inspect that spelling.
std::string emitBody(const HighFunc &Func, Arch Architecture = Arch::X64,
                     bool Portable = false) {
  std::string Output;
  llvm::raw_string_ostream OS(Output);
  CEmitterOptions Options;
  Options.TheArch = Architecture;
  Options.UseUnalignedPointers = !Portable;
  EXPECT_TRUE(HighCEmitter().emit({Func}, OS, Options));
  OS.flush();
  auto Body = functionBody(Output, Func.Name);
  EXPECT_TRUE(Body.has_value()) << Output;
  return Body ? Body->str() : Output;
}

HighStmt store(ExprPtr Address, ExprPtr Value) {
  HighStmt Result;
  Result.Kind = StmtKind::Store;
  Result.StoreAddr = std::move(Address);
  Result.StoreVal = std::move(Value);
  return Result;
}

HighStmt ret(ExprPtr Value) {
  HighStmt Result;
  Result.Kind = StmtKind::Return;
  Result.RetVal = std::move(Value);
  return Result;
}

TEST(HighCStoreForwarding, BitwiseOperandsParenthesizeComparisons) {
  auto U64 = NdType::makeInt(8, false);
  for (NdOp Bitwise : {NdOp::INT_AND, NdOp::INT_OR, NdOp::INT_XOR}) {
    for (NdOp Compare : {NdOp::INT_NOTEQUAL, NdOp::INT_LESS}) {
      for (bool OnRight : {false, true}) {
        HighFunc Func;
        Func.Name = "compared_bits";
        Func.ReturnType = U64;
        Func.Params = {{"arg0", U64}, {"arg1", U64}};
        auto Condition = HighExpr::makeBinop(Compare, makeParam(0, 8, U64),
                                             HighExpr::makeConst(3, 8));
        auto Mask = makeParam(1, 8, U64);
        Func.Body = {ret(HighExpr::makeBinop(
            Bitwise, OnRight ? Mask : Condition, OnRight ? Condition : Mask))};
        const auto Body = emitBody(Func);
        EXPECT_NE(Body.find(Compare == NdOp::INT_NOTEQUAL ? "(arg0 != 3)"
                                                          : "(arg0 < 3)"),
                  std::string::npos)
            << Body;
      }
    }
  }
}

TEST(HighCStoreForwarding, RetainsDefinitionsUsedByForwardedValues) {
  for (Arch Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
    for (unsigned Mode = 0; Mode != 4; ++Mode) {
      const bool FloatConversion = (Mode & 1) != 0;
      const bool AdditionalUse = (Mode & 2) != 0;
      SCOPED_TRACE(static_cast<unsigned>(Architecture));
      SCOPED_TRACE(Mode);
      const auto I32 = NdType::makeInt(4);
      const auto U64 = NdType::makeInt(8, false);
      const auto F64 = NdType::makeFloat(8);
      HighFunc Func;
      Func.Name = "forwarded_value_definition";
      Func.FrameSize = 16;
      Func.ReturnType = FloatConversion ? F64 : I32;
      Func.Params = {{"arg0", I32}};
      MedVar Input;
      Input.Kind = MedVar::Temp;
      Input.Id = 17;
      Input.Size = 4;
      Input.TheArch = Architecture;
      HighStmt Define;
      Define.Kind = StmtKind::Assign;
      Define.Dst = HighExpr::makeVar(Input, I32);
      Define.Val = makeParam(0, 4, I32, Architecture);
      Func.Body.push_back(std::move(Define));
      ExprPtr Value = HighExpr::makeVar(Input, I32);
      if (FloatConversion) {
        Value = HighExpr::makeUnary(
            NdOp::FLOAT_FLOAT2FLOAT,
            HighExpr::makeBitCast(Value, NdType::makeFloat(4)));
        Value->Type = F64;
        Value = HighExpr::makeBitCast(Value, U64);
      }
      Func.Body.push_back(store(frameSlot(16, Architecture), Value));
      ExprPtr Loaded = HighExpr::makeLoad(frameSlot(16, Architecture),
                                          FloatConversion ? U64 : I32);
      if (FloatConversion)
        Loaded = HighExpr::makeBitCast(Loaded, F64);
      if (AdditionalUse) {
        ExprPtr Extra = HighExpr::makeVar(Input, I32);
        if (FloatConversion) {
          Extra = HighExpr::makeUnary(
              NdOp::FLOAT_FLOAT2FLOAT,
              HighExpr::makeBitCast(Extra, NdType::makeFloat(4)));
          Extra->Type = F64;
        }
        Loaded = HighExpr::makeBinop(
            FloatConversion ? NdOp::FLOAT_ADD : NdOp::INT_ADD, Loaded, Extra);
      }
      Func.Body.push_back(ret(Loaded));

      const std::string Body = emitBody(Func, Architecture);
      // The forwarded expression still uses the earlier copy. It must either
      // retain that copy's declaration and assignment or substitute its source
      // consistently; deleting the copy alone prints an undefined identifier.
      if (Body.find("t17") != std::string::npos) {
        EXPECT_NE(Body.find("int32_t t17;"), std::string::npos) << Body;
        EXPECT_NE(Body.find("t17 = arg0;"), std::string::npos) << Body;
        EXPECT_LT(Body.find("t17 = arg0;"), Body.find("return ")) << Body;
      } else {
        EXPECT_NE(Body.find("arg0"), std::string::npos) << Body;
      }
      // This must exercise forwarding, rather than pass by disabling it.
      EXPECT_EQ(countPointerDerefStores(Body), 0u) << Body;
      EXPECT_EQ(countPointerDerefLoads(Body), 0u) << Body;
    }
  }
}

TEST(MedToHighMemoryAddress, NarrowsOnlyProvenZeroExtendedPointerCarriers) {
  for (Arch Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
    for (unsigned Mode = 0; Mode != 5; ++Mode) {
      for (bool Atomic : {false, true}) {
        SCOPED_TRACE(static_cast<unsigned>(Architecture));
        SCOPED_TRACE(Mode);
        SCOPED_TRACE(Atomic);
        const auto &TRI = getTargetRegInfo(Architecture);
        MedFunc Med;
        Med.Entry = 0x1000;
        Med.Name = "pointer_carrier";
        Med.ReturnType = NdType::makeInt(4, false);
        Med.ReturnValueEvidence = MedReturnValueEvidence::ReturnsInteger;
        auto Variable = [&](MedVar::VarKind Kind, int Id, uint16_t Size) {
          MedVar V;
          V.Kind = Kind;
          V.Id = Id;
          V.SSAVer = 1;
          V.Size = Size;
          V.TheArch = Architecture;
          return V;
        };
        MedVar Input = Variable(MedVar::Param, 0, 4);
        MedVar Value = Variable(MedVar::Param, 1, 4);
        Med.Params = {Input, Value};
        Med.Blocks.emplace_back();
        MedBlock &Block = Med.Blocks.back();
        Block.Id = 0;
        Block.StartAddr = Med.Entry;
        Block.EndAddr = Med.Entry + 32;
        auto Append = [&](NdOp Opcode, MedVar Output,
                          std::initializer_list<MedVar> Inputs) {
          MedOp Op;
          Op.Opcode = Opcode;
          Op.Output = Output;
          Op.Addr = Med.Entry + Block.Ops.size();
          for (const MedVar &Operand : Inputs)
            Op.addInput(Operand);
          Block.Ops.push_back(std::move(Op));
        };
        MedVar Address = Variable(MedVar::Temp, 2, 8);
        Append(
            Mode == 1 ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Address,
            {Mode == 4 ? MedVar::makeConst(UINT64_C(0x80000000), 4) : Input});
        if (Mode == 2) {
          MedVar Wide = Variable(MedVar::Temp, 3, 8);
          Append(NdOp::INT_ADD, Wide,
                 {Address, MedVar::makeConst(UINT64_C(0x100000000), 8)});
          Address = Wide;
        } else if (Mode == 3) {
          Address = MedVar::makeConst(UINT64_C(0x180000000), 8);
        }
        Append(NdOp::STORE, {}, {Address, Value});
        MedVar Result = Variable(MedVar::Reg, 4, 4);
        Result.RegOff = TRI.IntReturnReg;
        if (Atomic)
          Append(NdOp::ATOMIC_ADD, Result, {Address, Value});
        else
          Append(NdOp::LOAD, Result, {Address});
        Append(NdOp::RETURN, {}, {Result});

        const HighFunc High = MedToHighConverter().convert(Med, Architecture);
        unsigned MemoryAddresses = 0;
        auto CheckAddress = [&](const ExprPtr &AddressExpr,
                                bool InlineDefinition = true) {
          const bool Narrowed = TRI.PointerSize == 4 &&
                                (Mode == 0 || Mode == 4) && InlineDefinition;
          ASSERT_NE(AddressExpr, nullptr);
          ASSERT_NE(AddressExpr->Type, nullptr);
          ++MemoryAddresses;
          EXPECT_EQ(AddressExpr->Type->Size, Narrowed ? 4 : 8)
              << AddressExpr->str();
          // The view is unsigned so source values >= 0x80000000 retain their
          // zero-extended address, rather than becoming negative host VAs.
          if (Narrowed)
            EXPECT_FALSE(AddressExpr->Type->IsSigned) << AddressExpr->str();
          if (Mode == 3) {
            ASSERT_EQ(AddressExpr->Kind, ExprKind::Const);
            EXPECT_EQ(AddressExpr->ConstVal, UINT64_C(0x180000000));
          } else if (Mode == 4) {
            const HighExpr *Constant = AddressExpr.get();
            while ((Constant->Kind == ExprKind::Cast ||
                    Constant->Kind == ExprKind::BitCast) &&
                   Constant->Operands.size() == 1 && Constant->Operands[0]) {
              ASSERT_NE(Constant->Type, nullptr);
              ASSERT_NE(Constant->Operands[0]->Type, nullptr);
              ASSERT_EQ(Constant->Type->Size,
                        Constant->Operands[0]->Type->Size);
              Constant = Constant->Operands[0].get();
            }
            if (Constant->Kind == ExprKind::UnaryOp) {
              // A shared atomic carrier and a 64-bit target retain the
              // original zero extension. Accept that exact operation, not a
              // sign extension or a numeric widening cast of 0x80000000.
              ASSERT_FALSE(Narrowed);
              ASSERT_EQ(Constant->Op, NdOp::INT_ZEXT);
              ASSERT_NE(Constant->Type, nullptr);
              ASSERT_EQ(Constant->Type->Size, 8u);
              ASSERT_EQ(Constant->Operands.size(), 1u);
              ASSERT_NE(Constant->Operands[0], nullptr);
              ASSERT_NE(Constant->Operands[0]->Type, nullptr);
              ASSERT_EQ(Constant->Operands[0]->Type->Size, 4u);
              Constant = Constant->Operands[0].get();
            }
            ASSERT_EQ(Constant->Kind, ExprKind::Const) << AddressExpr->str();
            EXPECT_EQ(Constant->ConstVal, UINT64_C(0x80000000));
          }
        };
        walkStmts(High.Body, [&](const HighStmt &Stmt) {
          if (Stmt.Kind == StmtKind::Store)
            CheckAddress(Stmt.StoreAddr);
          forEachRhsExpr(Stmt, [&](const ExprPtr &Root) {
            auto Visit = [&](auto &&Self, const ExprPtr &Expr) -> void {
              if (!Expr)
                return;
              if (Expr->Kind == ExprKind::Load ||
                  (Expr->Kind == ExprKind::BinOp &&
                   Expr->Op == NdOp::ATOMIC_ADD)) {
                ASSERT_FALSE(Expr->Operands.empty());
                CheckAddress(Expr->Operands[0], !Atomic);
              }
              Expr->forEachChildExpr(
                  [&](const ExprPtr &Child) { Self(Self, Child); });
            };
            Visit(Visit, Root);
          });
        });
        EXPECT_EQ(MemoryAddresses, 2u);
      }
    }
  }
}

TEST(HighCStoreForwarding, CallResultsFollowRenderedLoadsAndCallArity) {
  // Exercise the liveness boundary with a certified cached value. The current
  // store producer conservatively excludes calls; this verifies the consumer
  // does not drop result definitions if forwarding facts are supplied.
  for (unsigned Mode = 0; Mode != 6; ++Mode) {
    SCOPED_TRACE(Mode);
    const auto I32 = NdType::makeInt(4);
    MedVar Result;
    Result.Kind = MedVar::Temp;
    Result.Id = 17;
    Result.Size = 4;
    HighFunc Function;
    Function.Name = "cached_call_result";
    Function.ReturnType = I32;
    HighStmt Define;
    Define.Kind = StmtKind::Assign;
    Define.Dst = HighExpr::makeVar(Result, I32);
    Define.Val = HighExpr::makeCall("produce_value", 0x2000, {});
    Define.Val->Type = I32;
    Function.Body.push_back(Define);
    auto Address = frameSlot(16);
    auto Load = HighExpr::makeLoad(Address, I32);
    ExprPtr Use = Load;
    if (Mode == 1 || Mode == 2 || Mode == 3) {
      Use = HighExpr::makeCall("consume_value", 0x3000, {Load});
      Use->Type = I32;
      if (Mode == 3) {
        Use->IsIndirectCall = true;
        Use->IndirectTarget = Load;
      }
    } else if (Mode == 4) {
      Use = std::make_shared<HighExpr>();
      Use->Kind = ExprKind::Addr;
      Use->Type = NdType::makePtr(I32);
      Use->Operands = {Load};
    }
    Function.Body.push_back(ret(Use));
    HighCAnalysisState State;
    State.AddressKeys.emplace(Address.get(), "private-slot-16");
    State.ForwardedAddressDeps["private-slot-16"].insert("t17");
    if (Mode == 5)
      State.DeadStmts.insert(&Function.Body.back());
    const auto VarFn = [](const MedVar &Variable) {
      return "t" + std::to_string(Variable.Id);
    };
    const auto ArgLimit = [Mode](const HighExpr &Call) {
      return Mode == 2 || Mode == 3 ? size_t(0) : Call.Operands.size();
    };
    analyzeUnusedCallResults(State, Function, VarFn, ArgLimit);
    const bool ResultIsRendered = Mode == 0 || Mode == 1 || Mode == 3;
    EXPECT_EQ(State.OmittedCallResults.count(&Function.Body[0]),
              ResultIsRendered ? 0u : 1u);
  }
}

TEST(HighCStoreForwarding, CopyCyclesNeedAnObservableReader) {
  for (unsigned Mode = 0; Mode != 4; ++Mode) {
    SCOPED_TRACE(Mode);
    const TypeRef Integer = NdType::makeInt(8, false);
    auto Local = [&](int Id) {
      MedVar Variable;
      Variable.Kind = MedVar::Temp;
      Variable.Id = Id;
      Variable.Size = 8;
      return HighExpr::makeVar(Variable, Integer);
    };
    auto Assign = [&](int Id, ExprPtr Value) {
      HighStmt Statement;
      Statement.Kind = StmtKind::Assign;
      Statement.Dst = Local(Id);
      Statement.Val = std::move(Value);
      return Statement;
    };
    HighFunc Function;
    Function.Name = "copy_cycle";
    Function.ReturnType = Integer;
    Function.Body = {Assign(1, HighExpr::makeUndef(8)), Assign(2, Local(1)),
                     Assign(1, Local(2)),
                     Assign(2, HighExpr::makeCall("produce", 0x2000, {})),
                     ret(Local(1))};
    if (Mode >= 2) {
      HighStmt Read;
      Read.Kind = StmtKind::Call;
      Read.CallExpr = HighExpr::makeCall("consume", 0x3000, {Local(1)});
      if (Mode == 3) {
        Read.CallExpr->IsIndirectCall = true;
        Read.CallExpr->IndirectTarget = Local(1);
      }
      Function.Body.insert(Function.Body.end() - 1, std::move(Read));
    }
    HighCAnalysisState State;
    State.VoidReturns = Mode != 1;
    const auto VarFn = [](const MedVar &Variable) {
      return "t" + std::to_string(Variable.Id);
    };
    const auto ArgLimit = [Mode](const HighExpr &Call) {
      return Mode == 3 ? size_t(0) : Call.Operands.size();
    };
    analyzeUnusedAssigns(State, Function, VarFn, ArgLimit);
    analyzeUnusedCallResults(State, Function, VarFn, ArgLimit);
    for (size_t I = 0; I != 3; ++I)
      EXPECT_EQ(State.DeadStmts.count(&Function.Body[I]), Mode == 0 ? 1u : 0u);
    EXPECT_FALSE(State.DeadStmts.count(&Function.Body[3]));
    EXPECT_EQ(State.OmittedCallResults.count(&Function.Body[3]),
              Mode == 0 ? 1u : 0u);
  }
}

TEST(HighCStoreForwarding, KeepsObservableStoresAndPrecedingLoads) {
  const auto I32 = NdType::makeInt(4);
  const auto Ptr = NdType::makePtr(I32);
  for (bool ReadBeforeWrite : {false, true}) {
    HighFunc Func;
    Func.Name = "observable_write";
    Func.ReturnType = I32;
    Func.Params = {{"arg0", Ptr}, {"arg1", I32}};
    auto Address = [&] { return makeParam(0, 8, Ptr); };
    MedVar Old;
    Old.Kind = MedVar::Temp;
    Old.Id = 7;
    Old.Size = 4;
    if (ReadBeforeWrite) {
      HighStmt Load;
      Load.Kind = StmtKind::Assign;
      Load.Dst = HighExpr::makeVar(Old, I32);
      Load.Val = HighExpr::makeLoad(Address(), I32);
      Func.Body.push_back(Load);
    }
    Func.Body.push_back(store(Address(), makeParam(1, 4, I32)));
    Func.Body.push_back(ret(ReadBeforeWrite ? HighExpr::makeVar(Old, I32)
                                            : makeParam(1, 4, I32)));
    const std::string Body = emitBody(Func);
    EXPECT_EQ(countPointerDerefStores(Body), 1u) << Body;
    if (ReadBeforeWrite) {
      EXPECT_EQ(countPointerDerefLoads(Body), 1u) << Body;
      EXPECT_LT(Body.find("t7 ="), Body.find("= arg1")) << Body;
    }
  }
}

TEST(HighCStoreForwarding, DoesNotMoveFrameWritesBeforeLoadsOrAcrossBranches) {
  const auto I32 = NdType::makeInt(4);
  for (bool Branch : {false, true}) {
    HighFunc Func;
    Func.Name = "ordered_frame";
    Func.FrameSize = 8;
    Func.ReturnType = I32;
    Func.Params = {{"arg0", I32}};
    HighStmt Load;
    Load.Kind = StmtKind::Assign;
    MedVar Old;
    Old.Kind = MedVar::Temp;
    Old.Id = 7;
    Old.Size = 4;
    Load.Dst = HighExpr::makeVar(Old, I32);
    Load.Val = HighExpr::makeLoad(frameSlot(4), I32);
    if (Branch) {
      HighStmt If;
      If.Kind = StmtKind::If;
      If.Cond = makeParam(0, 4, I32);
      If.Body.push_back(store(frameSlot(4), makeParam(0, 4, I32)));
      Func.Body.push_back(std::move(If));
      Func.Body.push_back(std::move(Load));
    } else {
      Func.Body.push_back(std::move(Load));
      Func.Body.push_back(store(frameSlot(4), makeParam(0, 4, I32)));
    }
    Func.Body.push_back(ret(HighExpr::makeVar(Old, I32)));
    const auto Body = emitBody(Func);
    EXPECT_GE(countPointerDerefStores(Body), 1u) << Body;
    EXPECT_TRUE(countPointerDerefLoads(Body) > 0 ||
                Body.find("var_") != std::string::npos)
        << Body;
  }
}

TEST(HighCStoreForwarding, PartialOverlapAndEscapedFramesKeepTheirStores) {
  const auto I32 = NdType::makeInt(4);
  for (bool Escape : {false, true}) {
    HighFunc Func;
    Func.Name = "observable_frame";
    Func.FrameSize = 8;
    Func.ReturnType = NdType::makeInt(8);
    Func.Body.push_back(store(frameSlot(8), HighExpr::makeConst(42, 4)));
    Func.Body.push_back(
        ret(Escape ? frameSlot(8) : HighExpr::makeLoad(frameSlot(7), I32)));
    const auto Body = emitBody(Func);
    EXPECT_GE(countPointerDerefStores(Body), 1u) << Body;
  }
}

TEST(HighCStoreForwarding,
     AddressTakenSlotKeepsBackingStorageAndInitialization) {
  const auto I32 = NdType::makeInt(4);
  const auto Pointer = NdType::makePtr(I32);
  HighFunc Func;
  Func.Name = "address_taken_copy";
  Func.FrameSize = 8;
  Func.ReturnType = Pointer;
  Func.Params = {{"arg0", I32}};
  Func.Body.push_back(store(frameSlot(8), makeParam(0, 4, I32)));
  auto Address = std::make_shared<HighExpr>();
  Address->Kind = ExprKind::Addr;
  Address->Type = Pointer;
  Address->Operands.push_back(HighExpr::makeLoad(frameSlot(8), I32));
  Func.Body.push_back(ret(std::move(Address)));

  const auto Body = emitBody(Func, Arch::X64, /*Portable=*/true);
  // Escaped frame addresses use the common backing storage so the store and
  // returned pointer retain the same memory identity. The slot, read and
  // written alike, is a member of that storage, and the returned pointer
  // points to the member.
  EXPECT_NE(Body.find("} stack_storage;"), std::string::npos) << Body;
  EXPECT_NE(Body.find("stack_storage.var_m8 = arg0;"), std::string::npos)
      << Body;
  EXPECT_NE(Body.find("return (int32_t *)&stack_storage.var_m8;"),
            std::string::npos)
      << Body;
  // The member names every access, so nothing reads the storage's base.
  EXPECT_EQ(Body.find("frame_base"), std::string::npos) << Body;
}

TEST(HighCStoreForwarding, NamedPointerFrameSlotKeepsExplicitPointerBits) {
  const auto I32 = NdType::makeInt(4);
  const auto Pointer = NdType::makePtr(I32);
  HighFunc Func;
  Func.Name = "pointer_frame_slot";
  Func.FrameSize = 8;
  Func.ReturnType = Pointer;
  Func.Params = {{"arg0", Pointer}};
  Func.Body.push_back(store(frameSlot(8), makeParam(0, 8, Pointer)));
  Func.Body.push_back(ret(HighExpr::makeLoad(frameSlot(8), Pointer)));

  const auto Body = emitBody(Func);
  EXPECT_NE(Body.find("int32_t* var_m8;"), std::string::npos) << Body;
  EXPECT_NE(Body.find("var_m8 = (int32_t*)(uintptr_t)((uintptr_t)arg0);"),
            std::string::npos)
      << Body;
  EXPECT_NE(Body.find("return var_m8;"), std::string::npos) << Body;
}

TEST(HighCStoreForwarding,
     NamedPointerFrameLoadAssignedToIntegerUsesPointerBits) {
  const auto I64 = NdType::makeInt(8);
  const auto Pointer = NdType::makePtr(NdType::makeInt(4));
  MedVar Bits;
  Bits.Kind = MedVar::Temp;
  Bits.Id = 19;
  Bits.Size = 8;
  Bits.TheArch = Arch::X64;
  HighFunc Func;
  Func.Name = "pointer_frame_to_integer";
  Func.FrameSize = 8;
  Func.ReturnType = I64;
  Func.Params = {{"arg0", Pointer}};
  Func.Body.push_back(store(frameSlot(8), makeParam(0, 8, Pointer)));
  HighStmt Assign;
  Assign.Kind = StmtKind::Assign;
  Assign.Dst = HighExpr::makeVar(Bits, I64);
  // The machine carrier remains an integer even though the named frame slot
  // is projected as a pointer.
  Assign.Val = HighExpr::makeLoad(frameSlot(8), I64);
  Func.Body.push_back(std::move(Assign));
  Func.Body.push_back(ret(HighExpr::makeVar(Bits, I64)));

  const auto Body = emitBody(Func);
  EXPECT_NE(Body.find("t19 = (int64_t)(uintptr_t)(var_m8);"), std::string::npos)
      << Body;
}

TEST(HighCStoreForwarding,
     IntegerCarrierAssignedToNamedPointerFrameSlotUsesPointerBits) {
  const auto I64 = NdType::makeInt(8);
  const auto Pointer = NdType::makePtr(NdType::makeInt(4));
  HighFunc Func;
  Func.Name = "integer_to_pointer_frame";
  Func.FrameSize = 8;
  Func.ReturnType = Pointer;
  Func.Params = {{"arg0", Pointer}, {"arg1", I64}};
  // Establish the source-level slot type before the later machine-carrier
  // update to the same bytes.
  Func.Body.push_back(store(frameSlot(8), makeParam(0, 8, Pointer)));
  HighStmt Assign;
  Assign.Kind = StmtKind::Assign;
  Assign.Dst = HighExpr::makeLoad(frameSlot(8), I64);
  Assign.Val = makeParam(1, 8, I64);
  Func.Body.push_back(std::move(Assign));
  Func.Body.push_back(ret(HighExpr::makeLoad(frameSlot(8), Pointer)));

  const auto Body = emitBody(Func);
  EXPECT_NE(Body.find("var_m8 = (int32_t*)(uintptr_t)(arg1);"),
            std::string::npos)
      << Body;
}

TEST(HighCStoreForwarding, RequiresPrivateFullWidthAndImmutableSlots) {
  const auto I32 = NdType::makeInt(4);
  for (unsigned Variant = 0; Variant < 4; ++Variant) {
    SCOPED_TRACE(Variant);
    HighFunc Func;
    Func.Name = "memory_ownership_boundary";
    Func.FrameSize = 8;
    Func.ReturnType = I32;
    auto Address = [&] {
      if (Variant == 0) {
        auto Cast = std::make_shared<HighExpr>();
        Cast->Kind = ExprKind::Cast;
        Cast->Type = Cast->CastTo = NdType::makeInt(4);
        Cast->Operands.push_back(frameSlot(4));
        return Cast;
      }
      if (Variant == 1)
        return frameSlot(0); // Caller-owned memory above the local frame.
      return frameSlot(4);
    };
    Func.Body.push_back(store(Address(), HighExpr::makeConst(42, 4)));
    if (Variant == 2) {
      auto Call = std::make_shared<HighExpr>();
      Call->Kind = ExprKind::Call;
      Call->CallTarget = "memory_barrier";
      HighStmt Invoke;
      Invoke.Kind = StmtKind::ExprStmt;
      Invoke.Val = Call;
      Func.Body.push_back(std::move(Invoke));
    }
    if (Variant == 3)
      Func.Body.push_back(store(Address(), HighExpr::makeConst(43, 4)));
    Func.Body.push_back(ret(HighExpr::makeLoad(Address(), I32)));
    const auto Body = emitBody(Func);
    EXPECT_EQ(countPointerDerefStores(Body), Variant == 3 ? 2u : 1u) << Body;
    EXPECT_TRUE(countPointerDerefLoads(Body) > 0 ||
                Body.find("var_") != std::string::npos)
        << Body;
  }
}

TEST(HighCStoreForwarding,
     LaterFrameAssignmentCannotReclassifyAnExternalWrite) {
  const auto I32 = NdType::makeInt(4);
  const auto Ptr = NdType::makePtr(I32);
  HighFunc Func;
  Func.Name = "external_before_frame_alias";
  Func.FrameSize = 8;
  Func.ReturnType = I32;
  Func.Params = {{"arg0", Ptr}};
  Func.Body.push_back(store(makeParam(0, 8, Ptr), HighExpr::makeConst(42, 4)));
  HighStmt Assign;
  Assign.Kind = StmtKind::Assign;
  Assign.Dst = makeParam(0, 8, Ptr);
  Assign.Val = frameSlot(8);
  Func.Body.push_back(std::move(Assign));
  Func.Body.push_back(ret(HighExpr::makeConst(0, 4)));
  const auto Body = emitBody(Func);
  EXPECT_GE(countPointerDerefStores(Body), 1u) << Body;
  size_t Assignment = std::string::npos;
  llvm::StringRef Remaining(Body);
  while (!Remaining.empty()) {
    const auto [Line, Rest] = Remaining.split('\n');
    if (Line.trim().starts_with("arg0 = ")) {
      Assignment = Line.data() - Body.data();
      break;
    }
    Remaining = Rest;
  }
  ASSERT_NE(Assignment, std::string::npos) << Body;
  EXPECT_LT(Body.find("0x2A"), Assignment) << Body;
}

TEST(HighCStoreForwarding, ReinterpretsEachIntegerLoadAfterStoreTruncation) {
  for (uint16_t Size : {1, 2, 4, 8}) {
    for (bool StoreSigned : {false, true}) {
      SCOPED_TRACE(Size);
      SCOPED_TRACE(StoreSigned);
      const auto StoreType = NdType::makeInt(Size, StoreSigned);
      const auto LoadType = NdType::makeInt(Size, !StoreSigned);
      HighFunc Func;
      Func.Name = "forward_integer_interpretation";
      Func.FrameSize = 8;
      Func.ReturnType = NdType::makeInt(8);
      Func.Params = {{"arg0", StoreType}};
      Func.Body.push_back(store(frameSlot(8), makeParam(0, Size, StoreType)));
      Func.Body.push_back(ret(HighExpr::makeLoad(frameSlot(8), LoadType)));
      const auto Body = emitBody(Func);
      // For example, a stored uint8_t(255), reloaded as int8_t and then
      // returned as int64_t, is -1. Returning the forwarded arg0 gives 255.
      const auto Expected = "return (" + typeToC(LoadType) + ")((" +
                            typeToC(StoreType) + ")(arg0));";
      EXPECT_NE(Body.find(Expected), std::string::npos) << Body;
      EXPECT_EQ(countPointerDerefStores(Body), 0u) << Body;
      EXPECT_EQ(countPointerDerefLoads(Body), 0u) << Body;
    }
  }
}

TEST(HighCStoreForwarding, TruncatesPromotedArithmeticBeforeWideningTheResult) {
  for (uint16_t Size : {1, 2}) {
    for (bool Signed : {false, true}) {
      SCOPED_TRACE(Size);
      SCOPED_TRACE(Signed);
      const auto Type = NdType::makeInt(Size, Signed);
      HighFunc Func;
      Func.Name = "forward_promoted_arithmetic";
      Func.FrameSize = 8;
      Func.ReturnType = NdType::makeInt(8);
      Func.Params = {{"arg0", Type}, {"arg1", Type}};
      Func.Body.push_back(
          store(frameSlot(8),
                HighExpr::makeBinop(NdOp::INT_ADD, makeParam(0, Size, Type),
                                    makeParam(1, Size, Type))));
      Func.Body.push_back(ret(HighExpr::makeLoad(frameSlot(8), Type)));
      const auto Body = emitBody(Func);
      // uint8_t(250 + 10) must become 4 at the store boundary even when
      // the store and load have exactly the same type; C evaluates + as int.
      const auto Expected =
          "return (" + typeToC(Type) + ")((" + typeToC(Type) + ")(";
      EXPECT_NE(Body.find(Expected), std::string::npos) << Body;
      EXPECT_EQ(countPointerDerefStores(Body), 0u) << Body;
      EXPECT_EQ(countPointerDerefLoads(Body), 0u) << Body;
    }
  }
}

TEST(HighCStoreForwarding, KeepsNonIntegerReinterpretationInMemory) {
  const auto I32 = NdType::makeInt(4);
  const auto F32 = NdType::makeFloat(4);
  const auto Ptr = NdType::makePtr(I32);
  for (const auto &[StoreType, LoadType] :
       std::vector<std::pair<TypeRef, TypeRef>>{{I32, F32},
                                                {F32, I32},
                                                {F32, F32},
                                                {Ptr, NdType::makeInt(8)},
                                                {NdType::makeInt(8), Ptr}}) {
    SCOPED_TRACE(typeToC(StoreType) + " -> " + typeToC(LoadType));
    HighFunc Func;
    Func.Name = "forward_reinterpretation_boundary";
    Func.FrameSize = 8;
    Func.ReturnType = LoadType;
    Func.Params = {{"arg0", StoreType}};
    Func.Body.push_back(
        store(frameSlot(8), makeParam(0, StoreType->Size, StoreType)));
    Func.Body.push_back(ret(HighExpr::makeLoad(frameSlot(8), LoadType)));
    const auto Body = emitBody(Func);
    EXPECT_GE(countPointerDerefStores(Body), 1u) << Body;
    EXPECT_TRUE(countPointerDerefLoads(Body) > 0 ||
                Body.find("var_") != std::string::npos)
        << Body;
  }
}

TEST(HighCStoreForwarding, EquivalentFrameAddressesKeepStoresForPartialReads) {
  const auto I32 = NdType::makeInt(4);
  for (bool PartialRead : {false, true}) {
    SCOPED_TRACE(PartialRead);
    auto Subtracted = frameSlot(8);
    auto Added =
        HighExpr::makeBinop(NdOp::INT_ADD, frameSlot(0)->Operands[0],
                            HighExpr::makeConst(static_cast<uint64_t>(-8), 8));
    HighFunc Func;
    Func.Name = "forward_address_alias";
    Func.FrameSize = 8;
    Func.ReturnType = I32;
    Func.Body.push_back(store(Subtracted, HighExpr::makeConst(42, 4)));
    Func.Body.push_back(ret(HighExpr::makeBinop(
        NdOp::INT_ADD, HighExpr::makeLoad(Subtracted, I32),
        HighExpr::makeLoad(Added,
                           PartialRead ? NdType::makeInt(1, false) : I32))));
    const auto Body = emitBody(Func);
    if (PartialRead) {
      // One full-width reader cannot erase the bytes needed by a narrow one.
      EXPECT_GE(countPointerDerefStores(Body), 1u) << Body;
      EXPECT_GE(countPointerDerefLoads(Body) + countOccurrences(Body, "var_"),
                1u)
          << Body;
    } else {
      // Both spellings denote the same private slot and may share its value.
      EXPECT_EQ(countPointerDerefStores(Body), 0u) << Body;
      EXPECT_EQ(countPointerDerefLoads(Body) + countOccurrences(Body, "var_"),
                0u)
          << Body;
      EXPECT_EQ(countOccurrences(Body, "0x2A"), 2u) << Body;
    }
  }
}

TEST(HighCStoreForwarding, IncludesStoreAndLoadCastsInExpressionBudget) {
  constexpr size_t Limit = 16 * 1024;
  const auto U8 = NdType::makeInt(1, false);
  const auto I8 = NdType::makeInt(1, true);
  const size_t StoreCastBytes = typeToC(U8).size() + 4;
  const size_t LoadCastBytes = typeToC(I8).size() + 4;
  for (bool ExceedsBudget : {false, true}) {
    SCOPED_TRACE(ExceedsBudget);
    HighFunc Func;
    Func.Name = "forward_cast_budget";
    Func.FrameSize = 8;
    Func.ReturnType = NdType::makeInt(8);
    Func.Params = {{"arg0", U8}};
    Func.Body.push_back(store(frameSlot(8), makeParam(0, 1, U8)));
    Func.Body.push_back(ret(HighExpr::makeLoad(frameSlot(8), I8)));
    const size_t ValueBytes =
        Limit - StoreCastBytes - LoadCastBytes + ExceedsBudget;
    auto VarFn = [](const MedVar &Var) {
      return Var.Kind == MedVar::Param ? "arg0" : "frame_base";
    };
    auto ExprFn = [&](const HighExpr &Expr) {
      if (Expr.Kind == ExprKind::Var && Expr.Var.Kind == MedVar::Param)
        return std::string(ValueBytes, 'x');
      return std::string("slot");
    };
    HighCAnalysisState State;
    analyzeDeadStores(State, Func, VarFn, ExprFn);
    ASSERT_TRUE(State.CanElideFrameStores);
    analyzeStoreForwarding(State, Func, VarFn, ExprFn);
    EXPECT_EQ(State.StoreFwd.empty(), ExceedsBudget);
    EXPECT_EQ(State.DeadStmts.count(&Func.Body[0]), !ExceedsBudget);
    if (!State.StoreFwd.empty()) {
      // The final read cast lives in the expression writer rather than the
      // cached store value; both still consume the same expression budget.
      EXPECT_EQ(State.StoreFwd.at("slot").size() + LoadCastBytes, Limit);
    }
  }
}

TEST(HighCStoreForwarding, BoundsTotalExpansionAcrossAllReaders) {
  const auto I8 = NdType::makeInt(1);
  HighFunc Func;
  Func.Name = "forward_total_budget";
  Func.FrameSize = 8;
  Func.ReturnType = NdType::makeInt(8);
  Func.Params = {{"arg0", I8}, {"arg1", I8}};
  Func.Body.push_back(store(frameSlot(8), makeParam(0, 1, I8)));
  Func.Body.push_back(store(frameSlot(4), makeParam(1, 1, I8)));
  Func.Body.push_back(ret(HighExpr::makeBinop(
      NdOp::INT_ADD, HighExpr::makeLoad(frameSlot(8), I8),
      HighExpr::makeBinop(NdOp::INT_ADD, HighExpr::makeLoad(frameSlot(4), I8),
                          HighExpr::makeLoad(frameSlot(4), I8)))));
  auto VarFn = [](const MedVar &Var) {
    return Var.Kind == MedVar::Param ? "arg" + std::to_string(Var.Id)
                                     : std::string("frame_base");
  };
  auto ExprFn = [](const HighExpr &Expr) {
    if (Expr.Kind == ExprKind::Var && Expr.Var.Kind == MedVar::Param)
      return std::string(6000, 'x');
    if (Expr.Kind == ExprKind::BinOp && Expr.Op == NdOp::INT_SUB)
      return "slot" + std::to_string(Expr.Operands[1]->ConstVal);
    return std::string("value");
  };
  HighCAnalysisState State;
  analyzeDeadStores(State, Func, VarFn, ExprFn);
  ASSERT_TRUE(State.CanElideFrameStores);
  analyzeStoreForwarding(State, Func, VarFn, ExprFn);
  // Each cached value fits 16 KiB and their sum also fits, but substituting
  // all three reads does not. One real store/load boundary must remain.
  ASSERT_EQ(State.StoreFwd.size(), 1u);
  EXPECT_EQ(State.DeadStmts.count(&Func.Body[0]) +
                State.DeadStmts.count(&Func.Body[1]),
            1u);
  const auto &[Addr, Value] = *State.StoreFwd.begin();
  const size_t Readers = Addr == "slot4" ? 2 : 1;
  EXPECT_LE((Value.size() + typeToC(I8).size() + 4) * Readers, 16u * 1024u);
}

TEST(HighCStoreForwarding, KeepsCyclicDependenciesMaterialized) {
  auto I32 = NdType::makeInt(4);
  auto I32Ptr = NdType::makePtr(I32);

  HighFunc Func;
  Func.Name = "cyclic_store_forwarding";
  Func.ReturnType = I32;
  Func.Params.push_back({"arg0", I32Ptr});

  auto Slot = [&](unsigned Index) {
    return HighExpr::makeBinop(NdOp::INT_ADD, makeParam(0, 8, I32Ptr),
                               HighExpr::makeConst(Index * 4, 8));
  };

  HighStmt StoreA;
  StoreA.Kind = StmtKind::Store;
  StoreA.StoreAddr = Slot(0);
  StoreA.StoreVal = HighExpr::makeLoad(Slot(1), I32);
  Func.Body.push_back(std::move(StoreA));

  HighStmt StoreB;
  StoreB.Kind = StmtKind::Store;
  StoreB.StoreAddr = Slot(1);
  StoreB.StoreVal = HighExpr::makeLoad(Slot(0), I32);
  Func.Body.push_back(std::move(StoreB));

  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = HighExpr::makeLoad(Slot(0), I32);
  Func.Body.push_back(std::move(Return));

  std::string Output;
  llvm::raw_string_ostream OS(Output);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  ASSERT_TRUE(HighCEmitter().emit({Func}, OS, Options));
  OS.flush();

  auto Body = functionBody(Output, Func.Name);
  ASSERT_TRUE(Body.has_value());
  EXPECT_LT(Body->size(), 4u * 1024u) << Body->take_front(4096).str();
  EXPECT_GE(countPointerDerefStores(*Body), 2u) << Body->take_front(4096).str();
  EXPECT_GE(countPointerDerefLoads(*Body) + countOccurrences(*Body, "var_"), 2u)
      << Body->take_front(4096).str();
}

TEST(HighCStoreForwarding, ByteStorageDoesNotPrintReplacedSlotNames) {
  // A frame address escapes into memory, so the frame becomes byte storage
  // after slot names were chosen. A temp that aliased such a slot must print
  // the storage access, not the slot name that is no longer declared.
  const auto I32 = NdType::makeInt(4);
  const auto I16 = NdType::makeInt(2);
  const auto I64 = NdType::makeInt(8);
  HighFunc Func;
  Func.Name = "storage_alias";
  Func.FrameSize = 0x40;
  Func.ReturnType = I32;
  Func.Params = {{"arg0", NdType::makePtr(I32)}, {"arg1", I32}};
  MedVar Base;
  Base.Kind = MedVar::Temp;
  Base.Id = 3;
  Base.Size = 8;
  MedVar Old;
  Old.Kind = MedVar::Temp;
  Old.Id = 25;
  Old.SSAVer = 1;
  Old.Size = 2;
  auto At = [&](uint64_t Offset) {
    return HighExpr::makeBinop(NdOp::INT_SUB, HighExpr::makeVar(Base, I64),
                               HighExpr::makeConst(Offset, 8));
  };
  HighStmt SetBase;
  SetBase.Kind = StmtKind::Assign;
  SetBase.Dst = HighExpr::makeVar(Base, I64);
  SetBase.Val = frameSlot(0x18);
  HighStmt LoadOld;
  LoadOld.Kind = StmtKind::Assign;
  LoadOld.Dst = HighExpr::makeVar(Old, I16);
  LoadOld.Val = HighExpr::makeLoad(At(16), I16);
  Func.Body = {
      SetBase, store(At(8), At(16)), LoadOld,
      store(At(16),
            HighExpr::makeBinop(NdOp::INT_AND, HighExpr::makeVar(Old, I16),
                                HighExpr::makeConst(0, 2))),
      ret(HighExpr::makeLoad(makeParam(0, 8, NdType::makePtr(I32)), I32))};
  const std::string Body = emitBody(Func);
  // Every slot name the body prints is declared in it.
  size_t From = 0;
  while ((From = Body.find("var_", From)) != std::string::npos) {
    size_t End = From;
    while (End < Body.size() &&
           (std::isalnum(static_cast<unsigned char>(Body[End])) ||
            Body[End] == '_'))
      ++End;
    const std::string Name = Body.substr(From, End - From);
    EXPECT_TRUE(Body.find(" " + Name + ";") != std::string::npos ||
                Body.find(" " + Name + "[") != std::string::npos)
        << Name << " is not declared\n"
        << Body;
    From = End;
  }
}

} // namespace

TEST(HighCStoreForwarding, ALandingPadStoresTheExceptionTheUnwinderHandsIt) {
  // x86 code stores a call's result right after the call, so an unread
  // result is the value such a store takes.  A landing pad's store after a
  // call, as a cleanup's after __stack_chk_fail, takes the exception object
  // the unwinder hands the pad instead, which no call returns: neither a
  // call that never returns nor one that does gives it.
  auto I32 = NdType::makeInt(4);
  const auto &TRI = getTargetRegInfo(Arch::X86);
  for (const char *Callee : {"__stack_chk_fail", "release"}) {
    SCOPED_TRACE(Callee);
    HighFunc Func;
    Func.Name = "cleanup";
    Func.Entry = 0x1000;
    Func.FrameSize = 0x20;
    Func.ReturnType = NdType::makeVoid();

    MedVar Result;
    Result.Kind = MedVar::Reg;
    Result.Id = 1;
    Result.SSAVer = 1;
    Result.Size = 4;
    Result.TheArch = Arch::X86;
    Result.RegOff = TRI.IntReturnReg;
    HighStmt Call;
    Call.Kind = StmtKind::Assign;
    Call.Addr = 0x1010;
    Call.Dst = HighExpr::makeVar(Result, I32);
    Call.Val = HighExpr::makeCall(Callee, 0x2000, {});
    Call.Val->Type = I32;
    Func.Body.push_back(std::move(Call));

    MedVar Exception = Result;
    Exception.Kind = MedVar::EHException;
    Exception.Id = -1;
    Exception.SSAVer = 0;
    HighStmt Save;
    Save.Kind = StmtKind::Store;
    Save.Addr = 0x1015;
    Save.StoreAddr = frameSlot(0x10, Arch::X86);
    Save.StoreVal = HighExpr::makeVar(Exception, I32);
    Func.Body.push_back(std::move(Save));

    HighStmt Resume;
    Resume.Kind = StmtKind::Call;
    Resume.Addr = 0x101A;
    Resume.CallExpr = HighExpr::makeCall(
        "_Unwind_Resume", 0x2010,
        {HighExpr::makeLoad(frameSlot(0x10, Arch::X86), I32)});
    Func.Body.push_back(std::move(Resume));
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Func.Body.push_back(std::move(Return));

    std::string Output;
    llvm::raw_string_ostream OS(Output);
    CEmitterOptions Options;
    Options.TheArch = Arch::X86;
    Options.Format = BinaryFormat::ELF;
    ASSERT_TRUE(HighCEmitter().emit({Func}, OS, Options));
    OS.flush();
    const auto Body = functionBody(Output, Func.Name);
    ASSERT_TRUE(Body.has_value()) << Output;
    EXPECT_FALSE(Body->contains(std::string("= ") + Callee + "("))
        << Body->str();
    EXPECT_TRUE(Body->contains("eh_exception")) << Body->str();
  }
}
