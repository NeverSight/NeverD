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
  C.source_id = "owned-fixture";
  C.external = SDK;
  return C;
}
std::vector<Class> fixture(bool SDK, bool Range, bool Alpha, bool Flag,
                           bool Varargs) {
  const std::string Object = "Ljava/lang/Object;", T = Alpha ? "Element" : "T",
                    U = Alpha ? "Key" : "U";
  auto Mapper = klass("Lfixture/Mapper;", true);
  Mapper.access = {"public", "interface", "abstract"};
  Mapper.generic_signature =
      "<A:Ljava/lang/Object;B:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Output = klass("Lfixture/Output;", true);
  Output.access = {"public", "interface", "abstract"};
  Output.generic_signature = "<O:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Provider = klass("Lfixture/Provider;", SDK);
  auto Parameters =
      Flag ? std::vector<std::string>{Mapper.name, "Z", "[Ljava/lang/Object;"}
           : std::vector<std::string>{Mapper.name, "[Ljava/lang/Object;"};
  auto Zero = op(0, "const/4", {0});
  Zero.literal = int64_t(0);
  auto Create = method({Provider.name, "create", Parameters, Output.name},
                       {"public", "static"}, Flag ? 4 : 3,
                       {Zero, op(1, "return-object", {0})});
  if (Varargs)
    Create.access.insert("varargs");
  Create.generic_signature = "<" + T + ":Ljava/lang/Object;" + U +
                             ":Ljava/lang/Object;>(Lfixture/Mapper<-T" + T +
                             ";+T" + U + ";>;" + (Flag ? "Z" : "") + "[T" + U +
                             ";)Lfixture/Output<T" + T + ";>;";
  if (SDK) {
    Create.instructions.clear();
    Create.registers = Create.code_end = 0;
  }
  Provider.methods.push_back(Create);
  auto Client = klass("Lfixture/Client;");
  Client.methods.push_back(method(
      {Client.name, "call", Parameters, Output.name}, {"public", "static"},
      Flag ? 4 : 3,
      {op(0, Range ? "invoke-static/range" : "invoke-static",
          Flag ? std::vector<unsigned>{1, 2, 3} : std::vector<unsigned>{1, 2},
          Create.reference),
       op(1, "move-result-object", {0}), op(2, "return-object", {0})}));
  auto Root = klass(Object, true);
  Root.superclass.reset();
  return {Provider, Client, Mapper, Output, Root};
}
} // namespace

TEST(MobileDalvikCallbackArrayFactory,
     RetainCompleteOutputArraysAndInvariantInputResults) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (bool Flag : {false, true})
          for (bool Varargs : {false, true})
            for (unsigned Route : {0u, 1u, 2u}) {
              SCOPED_TRACE(::testing::PrintToString(
                  std::tuple{SDK, Range, Alpha, Flag, Varargs, Route}));
              auto Input = fixture(SDK, Range, Alpha, Flag, Varargs);
              const auto Ref = Input[0].methods[0].reference;
              const auto Signature = Input[0].methods[0].generic_signature;
              const auto Flags = Input[0].methods[0].access;
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
              EXPECT_TRUE(Plans.erased_generic_invocations.contains(Ref));
              EXPECT_EQ(Classes.at(Ref.owner).methods[0].generic_signature,
                        Signature);
              EXPECT_EQ(Classes.at(Ref.owner).methods[0].access, Flags);
              const auto R = recoverJava(Classes, B);
              EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
              EXPECT_FALSE(R.getArray("generic_signature_projections"));
            }
}

TEST(MobileDalvikCallbackArrayFactory,
     RejectIncompleteDirectionScopeVariadicAndWordContracts) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 20; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{SDK, Mutation}));
      auto Input = fixture(SDK, true, false, true, true);
      auto &Create = Input[0].methods[0];
      auto &Call = Input[1].methods[0];
      const std::string Prefix = "<T:Ljava/lang/Object;U:Ljava/lang/Object;>",
                        Head = "(Lfixture/Mapper<-TT;+TU;>;Z",
                        Tail = ")Lfixture/Output<TT;>;";
      if (Mutation == 0)
        Create.generic_signature =
            Prefix + "(Lfixture/Mapper<-TT;+TT;>;Z[TU;" + Tail;
      else if (Mutation == 1)
        Create.generic_signature = Prefix + Head + "[TT;" + Tail;
      else if (Mutation == 2)
        Create.generic_signature = Prefix + Head + "[TU;)Lfixture/Output<TU;>;";
      else if (Mutation == 3)
        Create.generic_signature =
            "<T:Ljava/lang/Object;U:Ljava/lang/Object;V:Ljava/lang/Object;>" +
            Head + "[TU;" + Tail;
      else if (Mutation == 4)
        Create.generic_signature = Prefix + Head + "[TMissing;" + Tail;
      else if (Mutation == 5) {
        Create.reference.parameters[1] = "I";
        Call.reference.parameters[1] = "I";
        Create.generic_signature =
            Prefix + "(Lfixture/Mapper<-TT;+TU;>;I[TU;" + Tail;
        Call.instructions[0].reference = Create.reference;
      } else if (Mutation == 6)
        Create.generic_signature =
            Prefix + "(Lfixture/Mapper<+TT;-TU;>;Z[TU;" + Tail;
      else if (Mutation == 7)
        Create.generic_signature =
            Prefix + Head + "[TU;)Lfixture/Output<+TT;>;";
      else if (Mutation == 8)
        Create.generic_signature =
            Prefix + "(Lfixture/Mapper<TT;>;Z[TU;" + Tail;
      else if (Mutation == 9)
        Create.generic_signature =
            Prefix + Head + "[TU;)Lfixture/Output<TT;TU;>;";
      else if (Mutation == 10)
        Create.access.insert("native");
      else if (Mutation == 11)
        Create.access.insert("synthetic");
      else if (Mutation == 12)
        Create.access.insert("bridge");
      else if (Mutation == 13)
        Create.access = {"private", "static", "varargs"};
      else if (Mutation == 14)
        Create.access.erase("static");
      else if (Mutation == 15)
        Input[2].generic_signature =
            "<A:Ljava/lang/Number;B:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 16)
        Create.generic_signature =
            "<T:Ljava/lang/Object;>" + Head + "[TU;" + Tail;
      else if (Mutation == 17) {
        Create.reference.parameters[2] = "[[Ljava/lang/Object;";
        Call.reference.parameters[2] = "[[Ljava/lang/Object;";
        Create.generic_signature = Prefix + Head + "[[TU;" + Tail;
        Call.instructions[0].reference = Create.reference;
      } else if (Mutation == 18)
        Create.declared_throws = {"Ljava/lang/Exception;"};
      else
        Call.instructions[0].registers.pop_back();
      Budget B;
      EXPECT_THROW(recoverJava(linkClasses(Input, B), B), std::runtime_error);
    }
}
