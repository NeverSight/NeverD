//===- HighCReturnJoinTests.cpp - Results that reach a joined return ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// A RETURN whose block joins paths that all leave the same version of the
// return register returns that value: it was established before the paths
// parted, here by a call whose result outlives a branch around a store.  A
// RETURN that no path reaches with a write of the register returns the
// parameter that arrived in it.
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/BinaryLoading.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include <filesystem>
#include <initializer_list>
#include <string>
#include <vector>

using namespace neverd;

namespace {

/// The body of \p Name's definition in \p Source, or empty.
std::string definitionBody(const std::string &Source, const std::string &Name) {
  for (size_t At = Source.find(" " + Name + "("); At != std::string::npos;
       At = Source.find(" " + Name + "(", At + 1)) {
    const size_t LineEnd = Source.find('\n', At);
    if (LineEnd == std::string::npos || LineEnd == 0 ||
        Source[LineEnd - 1] != '{')
      continue;
    const size_t End = Source.find("\n}\n", LineEnd);
    return Source.substr(LineEnd,
                         End == std::string::npos ? End : End - LineEnd);
  }
  return {};
}

/// HighC for the function \p Name of fixture \p Object, or empty.
/// \p AlsoLift are same-object callers. A reader has to be lifted before a
/// result it uses can keep the callee from being settled as void.
std::string
decompileFixtureFunction(const char *Object, const char *Name,
                         std::initializer_list<const char *> AlsoLift = {}) {
  auto ImageOrErr = loadBinary(std::filesystem::path(TEST_OBJ_DIR) / Object);
  if (!ImageOrErr) {
    ADD_FAILURE() << llvm::toString(ImageOrErr.takeError());
    return {};
  }
  const BinaryImage &Img = *ImageOrErr;
  const Symbol *Entry = Img.findSymbol(Name);
  if (!Entry) {
    ADD_FAILURE() << "no symbol " << Name;
    return {};
  }
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries = {Entry->Addr};
  for (const char *Extra : AlsoLift) {
    const Symbol *Sym = Img.findSymbol(Extra);
    if (!Sym) {
      ADD_FAILURE() << "no symbol " << Extra;
      return {};
    }
    Opts.OnlyFunctionEntries.insert(Sym->Addr);
  }
  const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
  if (!Result.Success) {
    ADD_FAILURE() << Result.Error;
    return {};
  }
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Img.Arch;
  Options.Format = Img.Format;
  Options.Image = &Img;
  if (!HighCEmitter().emit(Result.HighFuncs, OS, Options)) {
    ADD_FAILURE() << "HighC emission failed";
    return {};
  }
  return Source;
}

TEST(HighCReturnJoin, ResultEstablishedBeforeABranchIsReturned) {
  for (const char *Object :
       {"test_return_join.o", "test_return_join_i386.o",
        "test_return_join_arm.o", "test_return_join_a64.o"}) {
    SCOPED_TRACE(Object);
    const std::string Source =
        decompileFixtureFunction(Object, "call_then_store");
    EXPECT_EQ(Source.find("void call_then_store("), std::string::npos)
        << Source;
    const std::string Body = definitionBody(Source, "call_then_store");
    ASSERT_FALSE(Body.empty()) << Source;
    EXPECT_NE(Body.find("triple("), std::string::npos) << Body;
    EXPECT_EQ(Body.find("return;"), std::string::npos) << Body;
  }
}

// `cbnz w0` after a call reads the result and does not write it. Both paths
// return the call. A compare of the incoming argument returns that argument
// at the width it arrived. A 32-bit write returns the value that write
// defined, including the zeros in the high half.
TEST(HighCReturnJoin, W0CompareDoesNotReplaceTheRegisterItRead) {
  const std::string Call = decompileFixtureFunction(
      "test_return_w0_view_a64.o", "call_result_survives_w0_test");
  const std::string CallBody =
      definitionBody(Call, "call_result_survives_w0_test");
  ASSERT_FALSE(CallBody.empty()) << Call;
  EXPECT_EQ(CallBody.find("unknown return value"), std::string::npos)
      << CallBody;
  EXPECT_NE(CallBody.find("w0_view_callee("), std::string::npos) << CallBody;
  const size_t CallReturn = CallBody.rfind("\n    return ");
  ASSERT_NE(CallReturn, std::string::npos) << CallBody;
  const std::string CallLine = CallBody.substr(CallReturn);
  EXPECT_EQ(CallLine.find("uint32_t"), std::string::npos) << CallLine;
  EXPECT_EQ(CallLine.find("int32_t"), std::string::npos) << CallLine;

  const std::string Arg = decompileFixtureFunction(
      "test_return_w0_view_a64.o", "w0_compare_returns_full_argument");
  const std::string ArgBody =
      definitionBody(Arg, "w0_compare_returns_full_argument");
  ASSERT_FALSE(ArgBody.empty()) << Arg;
  EXPECT_EQ(ArgBody.find("unknown return value"), std::string::npos) << ArgBody;
  EXPECT_NE(ArgBody.find("return 7;"), std::string::npos) << ArgBody;
  EXPECT_NE(ArgBody.find("return arg0;"), std::string::npos) << ArgBody;
  EXPECT_EQ(ArgBody.find("return (uint32_t)arg0"), std::string::npos)
      << ArgBody;
  EXPECT_EQ(ArgBody.find("return (int32_t)arg0"), std::string::npos) << ArgBody;
  EXPECT_EQ(ArgBody.find("return (int64_t)(uint32_t)arg0"), std::string::npos)
      << ArgBody;

  const std::string Written = decompileFixtureFunction(
      "test_return_w0_view_a64.o", "w_write_defines_the_result");
  const std::string WrittenBody =
      definitionBody(Written, "w_write_defines_the_result");
  ASSERT_FALSE(WrittenBody.empty()) << Written;
  EXPECT_EQ(WrittenBody.find("unknown return value"), std::string::npos)
      << WrittenBody;
  EXPECT_NE(WrittenBody.find("return 7;"), std::string::npos) << WrittenBody;
}

// `cbnz w0` joins before a later branch. The return is not in that join, and
// the later branch does not write x0. The call result still returns at the
// width the call wrote. The other side of the later branch is the constant.
// A side that overwrites x0 before the later branch stays part of the join.
TEST(HighCReturnJoin, CallResultSurvivesABranchAfterTheCompare) {
  const std::string Call = decompileFixtureFunction(
      "test_return_w0_view_a64.o", "call_result_survives_a_later_branch");
  const std::string CallBody =
      definitionBody(Call, "call_result_survives_a_later_branch");
  ASSERT_FALSE(CallBody.empty()) << Call;
  EXPECT_EQ(CallBody.find("unknown return value"), std::string::npos)
      << CallBody;
  EXPECT_NE(CallBody.find("w0_view_callee("), std::string::npos) << CallBody;
  EXPECT_NE(CallBody.find("return 7;"), std::string::npos) << CallBody;
  bool SawCallReturn = false;
  size_t At = 0;
  while (At < CallBody.size()) {
    const size_t End = CallBody.find('\n', At);
    const std::string Line = CallBody.substr(
        At, End == std::string::npos ? std::string::npos : End - At);
    At = End == std::string::npos ? CallBody.size() : End + 1;
    if (Line.find("return ") == std::string::npos ||
        Line.find("return 7;") != std::string::npos)
      continue;
    SawCallReturn = true;
    EXPECT_EQ(Line.find("uint32_t"), std::string::npos) << Line;
    EXPECT_EQ(Line.find("int32_t"), std::string::npos) << Line;
  }
  EXPECT_TRUE(SawCallReturn) << CallBody;
  EXPECT_EQ(CallBody.find("uint32_t"), std::string::npos) << CallBody;
  EXPECT_EQ(CallBody.find("int32_t"), std::string::npos) << CallBody;

  const std::string Over = decompileFixtureFunction(
      "test_return_w0_view_a64.o", "later_branch_keeps_an_overwriting_side");
  const std::string OverBody =
      definitionBody(Over, "later_branch_keeps_an_overwriting_side");
  ASSERT_FALSE(OverBody.empty()) << Over;
  EXPECT_EQ(OverBody.find("unknown return value"), std::string::npos)
      << OverBody;
  EXPECT_NE(OverBody.find("w0_view_callee("), std::string::npos) << OverBody;
  EXPECT_NE(OverBody.find("= 4;"), std::string::npos) << OverBody;
  EXPECT_NE(OverBody.find("return 7;"), std::string::npos) << OverBody;
  EXPECT_EQ(OverBody.find("return w0_view_callee("), std::string::npos)
      << OverBody;
}

/// \p Body returns \p Arg, rather than only passing it to a call.
bool returnsNamed(const std::string &Body, const std::string &Arg) {
  return Body.find("return " + Arg) != std::string::npos ||
         Body.find("return (" + Arg) != std::string::npos ||
         Body.find("? " + Arg) != std::string::npos ||
         Body.find(": " + Arg) != std::string::npos ||
         Body.find("= " + Arg) != std::string::npos ||
         Body.find("= (" + Arg) != std::string::npos ||
         Body.find("= (int32_t)" + Arg) != std::string::npos ||
         Body.find("= (int64_t)" + Arg) != std::string::npos ||
         Body.find("= (uint32_t)" + Arg) != std::string::npos ||
         Body.find("= (uint64_t)" + Arg) != std::string::npos;
}

// One side of the join calls. The other side never writes x0, so it returns
// the argument that arrived there. A load on that side is the loaded value.
// A 32-bit write on the other side stays that write. A declared void call
// keeps its effects and invalidates the result. The reader is lifted so a
// result with an observed producer is not settled as void.
TEST(HighCReturnJoin, ArgumentSurvivesBesideACall) {
  const std::string Source = decompileFixtureFunction(
      "test_return_w0_view_a64.o", "argument_survives_beside_a_call",
      {"read_argument_survives"});
  const std::string Body =
      definitionBody(Source, "argument_survives_beside_a_call");
  ASSERT_FALSE(Body.empty()) << Source;
  EXPECT_EQ(Source.find("void argument_survives_beside_a_call("),
            std::string::npos)
      << Source;
  EXPECT_EQ(Body.find("unknown return value"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("unknown value"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("return;"), std::string::npos) << Body;
  EXPECT_NE(Body.find("return_nine("), std::string::npos) << Body;
  EXPECT_TRUE(returnsNamed(Body, "arg0")) << Body;

  const std::string Saved = decompileFixtureFunction(
      "test_return_w0_view_a64.o", "saved_argument_beside_call",
      {"read_saved_argument"});
  const std::string SavedBody =
      definitionBody(Saved, "saved_argument_beside_call");
  ASSERT_FALSE(SavedBody.empty()) << Saved;
  size_t SavedSig = std::string::npos;
  for (size_t At = 0; (At = Saved.find(" saved_argument_beside_call(", At)) !=
                      std::string::npos;
       ++At) {
    const size_t LineEnd = Saved.find('\n', At);
    if (LineEnd != std::string::npos && LineEnd > 0 &&
        Saved[LineEnd - 1] == '{') {
      SavedSig = At;
      break;
    }
  }
  ASSERT_NE(SavedSig, std::string::npos) << Saved;
  const std::string SavedDecl =
      Saved.substr(SavedSig, Saved.find('\n', SavedSig) - SavedSig);
  EXPECT_NE(SavedDecl.find("int32_t arg0"), std::string::npos) << SavedDecl;
  EXPECT_EQ(SavedDecl.find("int64_t arg0"), std::string::npos) << SavedDecl;
  EXPECT_EQ(Saved.find("void saved_argument_beside_call("), std::string::npos)
      << Saved;
  EXPECT_EQ(SavedBody.find("unknown return value"), std::string::npos)
      << SavedBody;
  EXPECT_EQ(SavedBody.find("unknown value"), std::string::npos) << SavedBody;
  EXPECT_EQ(SavedBody.find("return;"), std::string::npos) << SavedBody;
  EXPECT_NE(SavedBody.find("return_nine("), std::string::npos) << SavedBody;
  EXPECT_TRUE(returnsNamed(SavedBody, "arg0")) << SavedBody;

  const std::string Written = decompileFixtureFunction(
      "test_return_w0_view_a64.o", "w_write_beside_argument", {"read_w_write"});
  const std::string WrittenBody =
      definitionBody(Written, "w_write_beside_argument");
  ASSERT_FALSE(WrittenBody.empty()) << Written;
  EXPECT_EQ(Written.find("void w_write_beside_argument("), std::string::npos)
      << Written;
  EXPECT_EQ(WrittenBody.find("unknown return value"), std::string::npos)
      << WrittenBody;
  EXPECT_EQ(WrittenBody.find("unknown value"), std::string::npos)
      << WrittenBody;
  EXPECT_EQ(WrittenBody.find("return;"), std::string::npos) << WrittenBody;
  EXPECT_NE(WrittenBody.find("7"), std::string::npos) << WrittenBody;
  EXPECT_TRUE(returnsNamed(WrittenBody, "arg0")) << WrittenBody;

  const std::string Voided = decompileFixtureFunction(
      "test_return_w0_view_a64.o", "void_call_is_not_the_argument",
      {"read_void_call", "clobber_x0", "drop_x0_result"});
  const std::string VoidBody =
      definitionBody(Voided, "void_call_is_not_the_argument");
  ASSERT_FALSE(VoidBody.empty()) << Voided;
  EXPECT_EQ(Voided.find("void void_call_is_not_the_argument("),
            std::string::npos)
      << Voided;
  EXPECT_NE(Voided.find("void clobber_x0("), std::string::npos) << Voided;
  EXPECT_NE(VoidBody.find("clobber_x0();"), std::string::npos) << VoidBody;
  EXPECT_EQ(VoidBody.find("return clobber_x0("), std::string::npos) << VoidBody;
  const auto CallAt = VoidBody.find("clobber_x0();");
  EXPECT_NE(VoidBody.find("_known = 0;", CallAt), std::string::npos)
      << VoidBody;
  EXPECT_NE(VoidBody.find("no recovered semantics", CallAt), std::string::npos)
      << VoidBody;
  EXPECT_EQ(VoidBody.find("unknown return value"), std::string::npos)
      << VoidBody;
  EXPECT_EQ(VoidBody.find("unknown value"), std::string::npos) << VoidBody;
  EXPECT_TRUE(returnsNamed(VoidBody, "arg0")) << VoidBody;

  const std::string Loaded = decompileFixtureFunction(
      "test_return_w0_view_a64.o", "loaded_side_is_not_the_argument");
  const std::string LoadedBody =
      definitionBody(Loaded, "loaded_side_is_not_the_argument");
  ASSERT_FALSE(LoadedBody.empty()) << Loaded;
  EXPECT_EQ(LoadedBody.find("unknown return value"), std::string::npos)
      << LoadedBody;
  EXPECT_EQ(LoadedBody.find("unknown value"), std::string::npos) << LoadedBody;
  EXPECT_NE(LoadedBody.find("return_nine("), std::string::npos) << LoadedBody;
  EXPECT_NE(LoadedBody.find("*"), std::string::npos) << LoadedBody;
  EXPECT_EQ(LoadedBody.find("return arg0;"), std::string::npos) << LoadedBody;
}

// The body never reads an argument register. The skip path still returns
// the x0 it was entered with, so that value is arg0. A void call on the
// other path does not.
TEST(HighCReturnJoin, UnreadArgumentSurvivesBesideACall) {
  const std::string Source = decompileFixtureFunction(
      "test_return_w0_view_a64.o", "unread_argument_survives_beside_a_call",
      {"read_unread_argument"});
  const std::string Body =
      definitionBody(Source, "unread_argument_survives_beside_a_call");
  ASSERT_FALSE(Body.empty()) << Source;
  EXPECT_EQ(Source.find("void unread_argument_survives_beside_a_call("),
            std::string::npos)
      << Source;
  EXPECT_NE(Source.find("int64_t arg0"), std::string::npos) << Source;
  EXPECT_EQ(Body.find("unknown return value"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("unknown value"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("return;"), std::string::npos) << Body;
  EXPECT_NE(Body.find("return_nine("), std::string::npos) << Body;
  EXPECT_TRUE(returnsNamed(Body, "arg0")) << Body;

  const std::string Voided = decompileFixtureFunction(
      "test_return_w0_view_a64.o", "unread_void_call_is_not_the_argument",
      {"read_unread_void_call", "clobber_x0", "drop_x0_result"});
  const std::string VoidBody =
      definitionBody(Voided, "unread_void_call_is_not_the_argument");
  ASSERT_FALSE(VoidBody.empty()) << Voided;
  EXPECT_EQ(Voided.find("void unread_void_call_is_not_the_argument("),
            std::string::npos)
      << Voided;
  EXPECT_NE(Voided.find("void clobber_x0("), std::string::npos) << Voided;
  EXPECT_NE(VoidBody.find("clobber_x0();"), std::string::npos) << VoidBody;
  EXPECT_EQ(VoidBody.find("return clobber_x0("), std::string::npos) << VoidBody;
  const auto CallAt = VoidBody.find("clobber_x0();");
  EXPECT_NE(VoidBody.find("_known = 0;", CallAt), std::string::npos)
      << VoidBody;
  EXPECT_NE(VoidBody.find("no recovered semantics", CallAt), std::string::npos)
      << VoidBody;
  EXPECT_EQ(VoidBody.find("unknown return value"), std::string::npos)
      << VoidBody;
  EXPECT_EQ(VoidBody.find("unknown value"), std::string::npos) << VoidBody;
  EXPECT_TRUE(returnsNamed(VoidBody, "arg0")) << VoidBody;
}

// A leaf that never writes x0 leaves the incoming argument live. The call
// after it receives that argument. A callee that itself calls writes x0, so
// the same argument does not reach the next call.
TEST(HighCReturnJoin, PreservedLeafForwardsIncomingX0) {
  const std::string Kept = decompileFixtureFunction(
      "test_return_w0_view_a64.o", "pass_x0_across_preserving_leaf",
      {"preserve_x0_leaf", "consume_live_x0"});
  const std::string KeptBody =
      definitionBody(Kept, "pass_x0_across_preserving_leaf");
  ASSERT_FALSE(KeptBody.empty()) << Kept;
  EXPECT_NE(KeptBody.find("consume_live_x0("), std::string::npos) << KeptBody;
  EXPECT_NE(KeptBody.find("consume_live_x0(arg0)"), std::string::npos)
      << KeptBody;
  EXPECT_EQ(KeptBody.find("unknown value"), std::string::npos) << KeptBody;

  const std::string Returned = decompileFixtureFunction(
      "test_return_w0_view_a64.o", "return_x0_after_preserving_leaf",
      {"preserve_x0_leaf", "read_preserved_return"});
  const std::string ReturnedBody =
      definitionBody(Returned, "return_x0_after_preserving_leaf");
  ASSERT_FALSE(ReturnedBody.empty()) << Returned;
  EXPECT_NE(ReturnedBody.find("return arg0;"), std::string::npos)
      << ReturnedBody;
  EXPECT_EQ(ReturnedBody.find("unknown return value"), std::string::npos)
      << ReturnedBody;
  EXPECT_EQ(ReturnedBody.find("unknown value"), std::string::npos)
      << ReturnedBody;

  const std::string Clobbered = decompileFixtureFunction(
      "test_return_w0_view_a64.o", "pass_x0_across_clobbering_call",
      {"clobber_x0_by_call", "drop_x0_result", "consume_live_x0"});
  const std::string ClobberedBody =
      definitionBody(Clobbered, "pass_x0_across_clobbering_call");
  ASSERT_FALSE(ClobberedBody.empty()) << Clobbered;
  EXPECT_NE(ClobberedBody.find("consume_live_x0("), std::string::npos)
      << ClobberedBody;
  EXPECT_EQ(ClobberedBody.find("consume_live_x0(arg0)"), std::string::npos)
      << ClobberedBody;
  EXPECT_NE(ClobberedBody.find("no recovered semantics"), std::string::npos)
      << ClobberedBody;
  EXPECT_EQ(ClobberedBody.find("unknown value"), std::string::npos)
      << ClobberedBody;
}

// AESE reads the incoming key. EOR reads an incoming Q the same way. A
// parameter, a write, a 64-bit half, and a store do not name that register.
// PACIA names an entry modifier and leaves an address use unknown.
TEST(HighCReturnJoin, EntryVectorAndPAuthModifierAreTheRegister) {
  const auto Body = [](const char *Name) {
    const std::string Source =
        decompileFixtureFunction("test_entry_register_a64.o", Name);
    const std::string Text = definitionBody(Source, Name);
    EXPECT_FALSE(Text.empty()) << Source;
    return Text;
  };
  const std::string Key = Body("entry_q_is_the_aes_key");
  EXPECT_EQ(Key.find("unknown register"), std::string::npos) << Key;
  EXPECT_NE(Key.find("mov %0.16b, v17.16b"), std::string::npos) << Key;

  const std::string Xor = Body("entry_q_xor_is_the_register");
  EXPECT_EQ(Xor.find("unknown register"), std::string::npos) << Xor;
  EXPECT_NE(Xor.find("mov %0.16b, v31.16b"), std::string::npos) << Xor;

  const std::string Args = Body("abi_vectors_stay_parameters");
  EXPECT_EQ(Args.find("unknown register"), std::string::npos) << Args;
  EXPECT_EQ(Args.find("mov %0.16b"), std::string::npos) << Args;

  const std::string Written = Body("written_q_is_not_the_entry_register");
  EXPECT_EQ(Written.find("unknown register"), std::string::npos) << Written;
  EXPECT_EQ(Written.find("mov %0.16b, v17.16b"), std::string::npos) << Written;

  const std::string Half = Body("d_half_stays_unknown");
  EXPECT_NE(Half.find("incoming register has no definition"), std::string::npos)
      << Half;
  EXPECT_EQ(Half.find("unknown register"), std::string::npos) << Half;
  EXPECT_EQ(Half.find("mov %0.16b"), std::string::npos) << Half;

  const std::string Stored = Body("stored_q_stays_unknown");
  EXPECT_NE(Stored.find("incoming register has no definition"),
            std::string::npos)
      << Stored;
  EXPECT_EQ(Stored.find("unknown register"), std::string::npos) << Stored;
  EXPECT_EQ(Stored.find("mov %0.16b"), std::string::npos) << Stored;

  const std::string Modifier = Body("pacia_names_entry_modifier");
  EXPECT_EQ(Modifier.find("unknown register"), std::string::npos) << Modifier;
  EXPECT_NE(Modifier.find("mov %0, x16"), std::string::npos) << Modifier;

  const std::string Param = Body("pacia_parameter_modifier_stays_argument");
  EXPECT_EQ(Param.find("unknown register"), std::string::npos) << Param;
  EXPECT_EQ(Param.find("mov %0, x1"), std::string::npos) << Param;

  const std::string Address = Body("address_use_stays_unknown");
  EXPECT_NE(Address.find("incoming register has no definition"),
            std::string::npos)
      << Address;
  EXPECT_EQ(Address.find("unknown register"), std::string::npos) << Address;
  EXPECT_EQ(Address.find("mov %0, x16"), std::string::npos) << Address;
}

// A machine-outlined sequence sets up no frame, so x29 is still its
// caller's frame pointer and `ldur w0, [x29, #-8]` reads the caller's slot.
// Its FDE gives it exception metadata, which makes that slot no local of its
// own that nothing assigns: the read fails closed on the incoming register.
TEST(HighCReturnJoin, AnOutlinedSequenceReadsItsCallersFrame) {
  if (!std::filesystem::exists(std::filesystem::path(TEST_OBJ_DIR) /
                               "test_outlined_caller_frame_a64.exe"))
    GTEST_SKIP() << "ld.lld did not link test_outlined_caller_frame_a64.exe";
  const std::string Source = decompileFixtureFunction(
      "test_outlined_caller_frame_a64.exe", "ocf_outlined");
  const std::string Text = definitionBody(Source, "ocf_outlined");
  ASSERT_FALSE(Text.empty()) << Source;
  EXPECT_EQ(Text.find("var_m8"), std::string::npos) << Text;
  EXPECT_NE(Text.find("incoming register has no definition"), std::string::npos)
      << Text;
}

// A deferred value prints as itself where its availability flag is the
// constant 1 on every path to that observation. The other arm, a join, and
// a loop header reached again after the flag is cleared stay traps.
TEST(HighCReturnJoin, DeferredFlagKnownSideIsTheValue) {
  auto Variable = [](int Id, MedVar::VarKind Kind = MedVar::Temp) {
    MedVar Value;
    Value.Kind = Kind;
    Value.Id = Id;
    Value.Size = 4;
    Value.TheArch = Arch::X64;
    return Value;
  };
  auto Word = NdType::makeInt(4, false);
  auto Assign = [](MedVar Dst, ExprPtr Value, bool Phi = false) {
    HighStmt Statement;
    Statement.Kind = StmtKind::Assign;
    Statement.Dst = HighExpr::makeVar(Dst, NdType::makeInt(4, false));
    Statement.Val = std::move(Value);
    Statement.IsPhiCopy = Phi;
    return Statement;
  };
  auto ReturnOf = [](MedVar Value) {
    HighStmt Statement;
    Statement.Kind = StmtKind::Return;
    Statement.RetVal = HighExpr::makeVar(Value, NdType::makeInt(4, false));
    return Statement;
  };
  auto Emit = [](HighFunc Function) {
    std::string Source;
    llvm::raw_string_ostream Output(Source);
    EXPECT_TRUE(HighCEmitter().emit({Function}, Output)) << Function.Name;
    return Source;
  };
  auto Returns = [](const std::string &Body) {
    std::vector<std::string> Lines;
    for (size_t At = 0; (At = Body.find("return ", At)) != std::string::npos;) {
      const size_t End = Body.find('\n', At);
      Lines.push_back(Body.substr(At, End - At));
      if (End == std::string::npos)
        break;
      At = End;
    }
    return Lines;
  };

  const MedVar Arg = Variable(0, MedVar::Param);
  const MedVar Value = Variable(1);
  const MedVar Copied = Variable(2);
  HighFunc Known;
  Known.Name = "known_side";
  Known.ReturnType = Word;
  Known.Params.push_back({"arg0", Word});
  HighStmt UnknownArm;
  UnknownArm.Kind = StmtKind::IfElse;
  UnknownArm.Cond = HighExpr::makeBinop(
      NdOp::INT_EQUAL, HighExpr::makeVar(Arg, Word), HighExpr::makeConst(0, 4));
  UnknownArm.Body = {Assign(Copied, HighExpr::makeVar(Value, Word), true),
                     ReturnOf(Copied)};
  HighStmt Use;
  Use.Kind = StmtKind::Block;
  Use.Body = {ReturnOf(Value)};
  UnknownArm.ElseBody = {Assign(Value, HighExpr::makeConst(7, 4)), Use};
  Known.Body = {Assign(Value, HighExpr::makeUndef(4), true), UnknownArm};
  const std::string KnownSource = Emit(std::move(Known));
  const std::string KnownBody = definitionBody(KnownSource, "known_side");
  ASSERT_FALSE(KnownBody.empty()) << KnownSource;
  const auto KnownLines = Returns(KnownBody);
  ASSERT_EQ(KnownLines.size(), 2u) << KnownBody;
  int Folded = 0;
  int Trapped = 0;
  for (const std::string &Line : KnownLines) {
    if (Line.find("no recovered semantics") == std::string::npos)
      ++Folded;
    else
      ++Trapped;
  }
  EXPECT_EQ(Folded, 1) << KnownBody;
  EXPECT_EQ(Trapped, 1) << KnownBody;
  EXPECT_NE(KnownBody.find("_known = 1"), std::string::npos) << KnownBody;
  EXPECT_EQ(KnownBody.find("unknown value"), std::string::npos) << KnownBody;
  // The folded return is the value, not the availability test.
  for (const std::string &Line : KnownLines)
    if (Line.find("no recovered semantics") == std::string::npos)
      EXPECT_EQ(Line.find("_known"), std::string::npos) << KnownBody;

  HighFunc Cleared;
  Cleared.Name = "cleared_side";
  Cleared.ReturnType = Word;
  Cleared.Params.push_back({"arg0", Word});
  HighStmt Clear;
  Clear.Kind = StmtKind::If;
  Clear.Cond = HighExpr::makeVar(Arg, Word);
  Clear.Body = {Assign(Value, HighExpr::makeUndef(4), true)};
  Cleared.Body = {Assign(Value, HighExpr::makeUndef(4), true),
                  Assign(Value, HighExpr::makeConst(7, 4)), Clear,
                  ReturnOf(Value)};
  const std::string ClearedSource = Emit(std::move(Cleared));
  const std::string ClearedBody = definitionBody(ClearedSource, "cleared_side");
  ASSERT_FALSE(ClearedBody.empty()) << ClearedSource;
  const auto ClearedReturn = ClearedBody.rfind("return ");
  ASSERT_NE(ClearedReturn, std::string::npos) << ClearedBody;
  const std::string ClearedLine = ClearedBody.substr(
      ClearedReturn, ClearedBody.find('\n', ClearedReturn) - ClearedReturn);
  EXPECT_NE(ClearedBody.find("= 7"), std::string::npos) << ClearedBody;
  EXPECT_NE(ClearedLine.find("no recovered semantics"), std::string::npos)
      << ClearedBody;
  EXPECT_EQ(ClearedBody.find("unknown value"), std::string::npos)
      << ClearedBody;
  EXPECT_NE(ClearedLine.find("_known"), std::string::npos) << ClearedLine;

  HighFunc Looped;
  Looped.Name = "loop_carried";
  Looped.ReturnType = Word;
  Looped.Params.push_back({"arg0", Word});
  HighStmt Loop;
  Loop.Kind = StmtKind::While;
  Loop.Cond = HighExpr::makeVar(Value, Word);
  Loop.Body = {Assign(Value, HighExpr::makeUndef(4), true)};
  Looped.Body = {Assign(Value, HighExpr::makeUndef(4), true),
                 Assign(Value, HighExpr::makeConst(7, 4)), Loop, ReturnOf(Arg)};
  const std::string LoopSource = Emit(std::move(Looped));
  const std::string LoopBody = definitionBody(LoopSource, "loop_carried");
  ASSERT_FALSE(LoopBody.empty()) << LoopSource;
  const auto WhileAt = LoopBody.find("while (");
  ASSERT_NE(WhileAt, std::string::npos) << LoopBody;
  const auto WhileBrace = LoopBody.find('{', WhileAt);
  ASSERT_NE(WhileBrace, std::string::npos) << LoopBody;
  const std::string WhileHeader =
      LoopBody.substr(WhileAt, WhileBrace - WhileAt);
  EXPECT_LT(LoopBody.find("= 7"), WhileAt) << LoopBody;
  EXPECT_NE(WhileHeader.find("no recovered semantics"), std::string::npos)
      << LoopBody;
  EXPECT_EQ(LoopBody.find("unknown value"), std::string::npos) << LoopBody;

  HighFunc Joined;
  Joined.Name = "joined_side";
  Joined.ReturnType = Word;
  Joined.Params.push_back({"arg0", Word});
  HighStmt Jump;
  Jump.Kind = StmtKind::If;
  Jump.Cond = HighExpr::makeVar(Arg, Word);
  HighStmt Goto;
  Goto.Kind = StmtKind::Goto;
  Goto.GotoTarget = 0x30;
  Jump.Body = {Goto};
  HighStmt Defined = Assign(Value, HighExpr::makeConst(7, 4));
  Defined.Addr = 0x20;
  HighStmt JoinReturn = ReturnOf(Value);
  JoinReturn.Addr = 0x30;
  Joined.Body = {Assign(Value, HighExpr::makeUndef(4), true), Jump, Defined,
                 JoinReturn};
  const std::string JoinSource = Emit(std::move(Joined));
  const std::string JoinBody = definitionBody(JoinSource, "joined_side");
  ASSERT_FALSE(JoinBody.empty()) << JoinSource;
  const auto Label = JoinBody.find("L_30:");
  ASSERT_NE(Label, std::string::npos) << JoinBody;
  const auto JoinReturnAt = JoinBody.find("return ", Label);
  ASSERT_NE(JoinReturnAt, std::string::npos) << JoinBody;
  const std::string JoinLine = JoinBody.substr(
      JoinReturnAt, JoinBody.find('\n', JoinReturnAt) - JoinReturnAt);
  EXPECT_LT(JoinBody.find("= 7"), Label) << JoinBody;
  EXPECT_NE(JoinLine.find("no recovered semantics"), std::string::npos)
      << JoinBody;
  EXPECT_EQ(JoinBody.find("unknown value"), std::string::npos) << JoinBody;
  EXPECT_NE(JoinLine.find("_known"), std::string::npos) << JoinLine;
}

// `if (b) return g(b); return a;`: on AArch64 and ARM the path that returns
// `a` is a bare return, since `a` arrives in the result register.
TEST(HighCReturnJoin, BareReturnBesideATailCallReturnsTheParameter) {
  for (const char *Object :
       {"test_return_join.o", "test_return_join_i386.o",
        "test_return_join_arm.o", "test_return_join_a64.o"})
    for (const char *Name : {"passthrough_direct", "passthrough_indirect"}) {
      SCOPED_TRACE(std::string(Object) + " " + Name);
      const std::string Source = decompileFixtureFunction(Object, Name);
      const std::string Body = definitionBody(Source, Name);
      ASSERT_FALSE(Body.empty()) << Source;
      EXPECT_EQ(Body.find("__builtin_trap"), std::string::npos) << Body;
      EXPECT_NE(Body.find("return arg0;"), std::string::npos) << Body;
    }
}

} // namespace
