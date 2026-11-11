#include "MobileDalvik.h"
#include "MobileDalvikSignature.h"
#include "gtest/gtest.h"

#include <algorithm>
#include <stdexcept>
#include <tuple>

using namespace neverd::mobile;
using namespace neverd::mobile::dalvik;

namespace {
Instruction op(unsigned PC, std::string Opcode,
               std::vector<unsigned> Registers = {}, Reference Ref = {}) {
  Instruction I;
  I.pc = PC;
  I.opcode = std::move(Opcode);
  I.registers = std::move(Registers);
  I.reference = std::move(Ref);
  return I;
}
Method method(MethodRef Ref, Access Flags, unsigned Registers,
              std::vector<Instruction> Code) {
  Method M;
  M.reference = std::move(Ref);
  M.access = std::move(Flags);
  M.registers = Registers;
  M.instructions = std::move(Code);
  M.code_end = M.instructions.empty() ? 0 : M.instructions.back().pc + 1;
  return M;
}
Class klass(std::string Name, bool SDK = false) {
  Class C;
  C.name = std::move(Name);
  C.superclass = "Ljava/lang/Object;";
  C.access = {"public"};
  C.external = SDK;
  C.source_id = "owned-fixture";
  return C;
}
void init(Class &C) {
  C.methods.push_back(method(
      {C.name, "<init>", {}, "V"}, {"public", "constructor"}, 1,
      {op(0, "invoke-direct", {0}, MethodRef{*C.superclass, "<init>", {}, "V"}),
       op(1, "return-void")}));
}
std::vector<Class> fixture(bool SDK, bool Range, bool Alpha, bool Incoming) {
  const std::string Object = "Ljava/lang/Object;", E = Alpha ? "Element" : "E";
  auto Base = klass("Lfixture/Base;");
  Base.generic_signature = "<B:Ljava/lang/Object;>Ljava/lang/Object;";
  init(Base);
  auto Mid = klass("Lfixture/Mid;");
  Mid.superclass = Base.name;
  Mid.generic_signature = "<M:Ljava/lang/Object;>Lfixture/Base<TM;>;";
  init(Mid);
  auto Bucket = klass("Lfixture/Bucket;");
  Bucket.superclass = Mid.name;
  Bucket.generic_signature = "<C:Ljava/lang/Object;>Lfixture/Mid<TC;>;";
  init(Bucket);
  auto Slot = klass("Lfixture/Slot;");
  Slot.access = {"public", "interface", "abstract"};
  Slot.generic_signature = "<S:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Context = klass("Lfixture/Context;");
  init(Context);
  auto Target = klass("Lfixture/Target;");
  Target.generic_signature = "<" + E + ":Ljava/lang/Object;>Ljava/lang/Object;";
  MethodRef Selected{Target.name, "<init>", {Base.name, Context.name}, "V"};
  auto SelectedMethod =
      method(Selected, {"public", "constructor"}, 3,
             {op(0, "invoke-direct", {0}, MethodRef{Object, "<init>", {}, "V"}),
              op(1, "return-void")});
  SelectedMethod.generic_signature =
      "(Lfixture/Base<Lfixture/Slot<T" + E + ";>;>;Lfixture/Context;)V";
  std::vector<Instruction> Code = {
      op(0, "move-object", {0, 3}), op(1, "new-instance", {1}, Bucket.name),
      op(2, Range ? "invoke-direct/range" : "invoke-direct", {1},
         Bucket.methods[0].reference)};
  if (Incoming)
    Code.push_back(op(3, "move-object", {2, 4}));
  else {
    Code.push_back(op(3, "new-instance", {2}, Context.name));
    Code.push_back(op(4, Range ? "invoke-direct/range" : "invoke-direct", {2},
                      Context.methods[0].reference));
  }
  Code.push_back(op(5, Range ? "invoke-direct/range" : "invoke-direct",
                    {0, 1, 2}, Selected));
  Code.push_back(op(6, "return-void"));
  Target.methods.push_back(
      method({Target.name, "<init>",
              Incoming ? std::vector<std::string>{Context.name}
                       : std::vector<std::string>{},
              "V"},
             {"public", "constructor"}, Incoming ? 5 : 4, Code));
  Target.methods.push_back(SelectedMethod);
  auto Root = klass(Object, true);
  Root.superclass.reset();
  Root.methods.push_back(
      method({Object, "<init>", {}, "V"}, {"public", "constructor"}, 0, {}));
  if (SDK)
    for (auto *C : {&Base, &Mid, &Bucket, &Slot, &Context}) {
      C->external = true;
      for (auto &M : C->methods) {
        M.instructions.clear();
        M.registers = M.code_end = 0;
      }
    }
  return {Target, Bucket, Context, Base, Mid, Slot, Root};
}
} // namespace

TEST(MobileDalvikAllocatedContext,
     RetainInitializedObjectsBesideCompleteScopedCarriers) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (bool Incoming : {false, true})
          for (unsigned Covariance : {0u, 1u, 2u})
            for (unsigned Route : {0u, 1u, 2u}) {
              SCOPED_TRACE(::testing::PrintToString(
                  std::tuple{SDK, Range, Alpha, Incoming, Covariance, Route}));
              auto Input = fixture(SDK, Range, Alpha, Incoming);
              if (Covariance) {
                auto &Signature = *Input[0].methods[1].generic_signature;
                const std::string Marker =
                    Covariance == 1 ? "Lfixture/Base<" : "Lfixture/Slot<";
                Signature.insert(Signature.find(Marker) + Marker.size(), "+");
              }
              const auto Caller = Input[0].methods[0].reference,
                         Selected = Input[0].methods[1].reference;
              const auto Original = Input[0].methods[1].generic_signature;
              if (Route == 2)
                std::reverse(Input.begin(), Input.end());
              Budget B;
              ClassMap Classes;
              if (Route)
                Classes = linkClasses(Input, B);
              else
                for (const auto &C : Input)
                  Classes.emplace(C.name, C);
              const auto Plans = validateGenericSignatures(Classes, B);
              EXPECT_TRUE(Plans.constructor_calls.contains({Caller, Selected}));
              EXPECT_EQ(Classes.at(Selected.owner).methods[1].generic_signature,
                        Original);
              const auto R = recoverJava(Classes, B);
              EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
              EXPECT_FALSE(R.getArray("generic_signature_projections"));
            }
}

TEST(MobileDalvikAllocatedContext,
     RejectIncompleteContractsAndChangedInitialization) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 16; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{SDK, Mutation}));
      auto Input = fixture(SDK, true, false, false);
      auto &Call = Input[0].methods[0];
      auto &Selected = Input[0].methods[1];
      const std::string Good =
          "(Lfixture/Base<Lfixture/Slot<TE;>;>;Lfixture/Context;)V";
      if (Mutation == 0)
        Input[2].generic_signature = "<C:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 1) {
        Selected.reference.parameters[1] = "Lfixture/Missing;";
        Selected.generic_signature =
            "(Lfixture/Base<Lfixture/Slot<TE;>;>;Lfixture/Missing;)V";
        Call.instructions[5].reference = Selected.reference;
      } else if (Mutation == 2)
        Selected.generic_signature =
            "(Lfixture/Base<Lfixture/Slot<-TE;>;>;Lfixture/Context;)V";
      else if (Mutation == 3)
        Selected.generic_signature =
            "(Lfixture/Base<-Lfixture/Slot<TE;>;>;Lfixture/Context;)V";
      else if (Mutation == 4)
        Selected.generic_signature =
            "(Lfixture/Base<Lfixture/Slot<TMissing;>;>;Lfixture/Context;)V";
      else if (Mutation == 5)
        Selected.generic_signature = "<U:Ljava/lang/Object;>" + Good;
      else if (Mutation == 6)
        Input[0].generic_signature = "<E:Ljava/lang/Number;>Ljava/lang/Object;";
      else if (Mutation == 7)
        Input[1].methods[0].access = {"private", "constructor"};
      else if (Mutation == 8)
        Input[2].methods[0].access.insert("varargs");
      else if (Mutation == 9)
        Input[2].methods[0].declared_throws = {"Ljava/lang/Exception;"};
      else if (Mutation == 10)
        Input[2].access.insert("abstract");
      else if (Mutation == 11)
        Call.instructions.erase(Call.instructions.begin() + 4);
      else if (Mutation == 12) {
        auto Lost = op(4, "const/4", {1});
        Lost.literal = int64_t(0);
        Call.instructions.insert(Call.instructions.begin() + 5, Lost);
      } else if (Mutation == 13)
        Call.instructions[5].registers.pop_back();
      else if (Mutation == 14)
        Selected.generic_signature =
            "(Lfixture/Base<Lfixture/Slot<TE;>;TE;>;Lfixture/Context;)V";
      else
        Call.instructions[5].opcode = "invoke-static/range";
      Budget B;
      EXPECT_THROW(recoverJava(linkClasses(Input, B), B), std::runtime_error);
    }
}
