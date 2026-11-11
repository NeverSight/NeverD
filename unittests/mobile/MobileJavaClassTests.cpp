#include "MobileDalvik.h"
#include "gtest/gtest.h"

using namespace neverd::mobile;
using namespace neverd::mobile::dalvik;

namespace {
void be(std::string &out, uint64_t value, unsigned bytes) {
  while (bytes)
    out += char(value >> (--bytes * 8));
}
struct ClassFile {
  std::string pool;
  unsigned count = 1;
  unsigned text(std::string value) {
    unsigned index = count++;
    be(pool, 1, 1);
    be(pool, value.size(), 2);
    pool += value;
    return index;
  }
  unsigned type(std::string name) {
    unsigned name_index = text(std::move(name)), index = count++;
    be(pool, 7, 1);
    be(pool, name_index, 2);
    return index;
  }
  std::string make(std::string name = "dependency/Base",
                   std::string signature = "") {
    auto self = type(name), super = type("java/lang/Object");
    auto method_name = text("value"), prototype = text("(I)I");
    auto signature_name = text("Signature"), signature_value = text(signature);
    std::string out;
    be(out, 0xcafebabe, 4);
    be(out, 0, 2);
    be(out, 52, 2);
    be(out, count, 2);
    out += pool;
    be(out, 0x421, 2); // public abstract class, ACC_SUPER
    be(out, self, 2);
    be(out, super, 2);
    be(out, 0, 2);     // interfaces
    be(out, 0, 2);     // fields
    be(out, 1, 2);     // methods
    be(out, 0x401, 2); // public abstract method
    be(out, method_name, 2);
    be(out, prototype, 2);
    be(out, 0, 2); // method attributes
    be(out, signature.empty() ? 0 : 1, 2);
    if (!signature.empty()) {
      be(out, signature_name, 2);
      be(out, 2, 4);
      be(out, signature_value, 2);
    }
    return out;
  }
  std::string annotationTargets(const std::vector<std::string> &targets) {
    auto self = type("dependency/Marker"), super = type("java/lang/Object");
    auto base = type("java/lang/annotation/Annotation");
    auto attr = text("RuntimeVisibleAnnotations");
    std::string metadata;
    be(metadata, 1, 2);
    be(metadata, text("Ljava/lang/annotation/Target;"), 2);
    be(metadata, 1, 2);
    be(metadata, text("value"), 2);
    be(metadata, '[', 1);
    be(metadata, targets.size(), 2);
    for (const auto &target : targets) {
      be(metadata, 'e', 1);
      be(metadata, text("Ljava/lang/annotation/ElementType;"), 2);
      be(metadata, text(target), 2);
    }
    std::string out;
    be(out, 0xcafebabe, 4);
    be(out, 0, 2);
    be(out, 61, 2);
    be(out, count, 2);
    out += pool;
    be(out, 0x2601, 2); // public abstract annotation interface
    be(out, self, 2);
    be(out, super, 2);
    be(out, 1, 2);
    be(out, base, 2);
    be(out, 0, 2); // fields
    be(out, 0, 2); // methods
    be(out, 1, 2);
    be(out, attr, 2);
    be(out, metadata.size(), 4);
    out += metadata;
    return out;
  }
  std::string interfaceRoles(unsigned major, unsigned flags,
                             std::optional<unsigned> inner_flags = {}) {
    const auto self =
        type(inner_flags ? "fixture/Outer$Member" : "fixture/LegacyRole");
    const auto super = type("java/lang/Object");
    const auto outer = type("fixture/Outer");
    const auto member = text("Member");
    const auto attribute = text("InnerClasses");
    std::string out;
    be(out, 0xcafebabe, 4);
    be(out, major == 45 ? 3 : 0, 2);
    be(out, major, 2);
    be(out, count, 2);
    out += pool;
    be(out, flags, 2);
    be(out, self, 2);
    be(out, super, 2);
    be(out, 0, 2); // interfaces
    be(out, 0, 2); // fields
    be(out, 0, 2); // methods
    be(out, inner_flags ? 1 : 0, 2);
    if (inner_flags) {
      be(out, attribute, 2);
      be(out, 10, 4);
      be(out, 1, 2);
      be(out, self, 2);
      be(out, outer, 2);
      be(out, member, 2);
      be(out, *inner_flags, 2);
    }
    return out;
  }
};

TEST(MobileJavaClass, LegacyInterfacesRetainImplicitAbstractRoles) {
  for (unsigned major : {45u, 46u, 48u, 49u})
    for (unsigned flags : {0x201u, 0x601u})
      for (std::optional<unsigned> inner :
           {std::optional<unsigned>{}, std::optional<unsigned>{0x209},
            std::optional<unsigned>{0x609}}) {
        SCOPED_TRACE(major);
        SCOPED_TRACE(flags);
        SCOPED_TRACE(inner.value_or(0));
        ClassFile fixture;
        Budget budget;
        const auto bytes = fixture.interfaceRoles(major, flags, inner);
        const auto cls =
            parseClassDeclaration(bytes, "owned-role.class", budget);
        EXPECT_EQ(cls.access, (Access{"public", "interface", "abstract"}));
        EXPECT_TRUE(cls.external);
        EXPECT_TRUE(cls.methods.empty());
        EXPECT_EQ(cls.inner_class_present, inner.has_value());
        if (inner)
          EXPECT_EQ(cls.inner_access,
                    (Access{"public", "static", "interface", "abstract"}));
        EXPECT_NO_THROW(linkClasses({cls}, budget));
      }
}

TEST(MobileJavaClass, LegacyRoleNormalizationKeepsModernAndInvalidFlagGuards) {
  for (unsigned major : {45u, 49u, 50u, 61u}) {
    SCOPED_TRACE(major);
    ClassFile concrete;
    Budget budget;
    const auto plain = parseClassDeclaration(
        concrete.interfaceRoles(major, 0x1), "owned-class.class", budget);
    EXPECT_EQ(plain.access, (Access{"public"}));
    EXPECT_NO_THROW(linkClasses({plain}, budget));
    ClassFile invalid;
    const auto final_interface = parseClassDeclaration(
        invalid.interfaceRoles(major, 0x211), "owned-final.class", budget);
    EXPECT_THROW(linkClasses({final_interface}, budget), Error);
    if (major < 50)
      continue;
    for (unsigned flags : {0x201u, 0x601u}) {
      ClassFile modern;
      const auto cls = parseClassDeclaration(
          modern.interfaceRoles(major, flags), "owned-modern.class", budget);
      EXPECT_EQ(cls.access.contains("abstract"), flags == 0x601);
      if (flags == 0x201)
        EXPECT_THROW(linkClasses({cls}, budget), Error);
      else
        EXPECT_NO_THROW(linkClasses({cls}, budget));
    }
    ClassFile member;
    const auto nested =
        parseClassDeclaration(member.interfaceRoles(major, 0x601, 0x209),
                              "owned-member.class", budget);
    EXPECT_FALSE(nested.inner_access.contains("abstract"));
  }
}

TEST(MobileJavaClass, AnnotationTargetsShareModelPolicy) {
  for (const auto &targets : std::vector<std::vector<std::string>>{
           {},
           {"TYPE"},
           {"MODULE"},
           {"RECORD_COMPONENT"},
           {"MODULE", "TYPE_USE", "RECORD_COMPONENT"}}) {
    ClassFile fixture;
    Budget budget;
    auto cls = parseClassDeclaration(fixture.annotationTargets(targets),
                                     "marker.class", budget);
    EXPECT_EQ(cls.annotation_metadata.targets, targets);
    // The same declaration must also validate when supplied by a DEX input.
    cls.external = false;
    EXPECT_NO_THROW(linkClasses({cls}, budget));
  }
  for (const auto &targets : std::vector<std::vector<std::string>>{
           {"UNKNOWN"}, {"module"}, {"TYPE", "TYPE"}, {"MODULE", "MODULE"}}) {
    ClassFile fixture;
    Budget budget;
    EXPECT_THROW(parseClassDeclaration(fixture.annotationTargets(targets),
                                       "marker.class", budget),
                 Error);
  }
}

TEST(MobileJavaClass, ReadsDependencyDeclarationsWithoutInventingBodies) {
  ClassFile fixture;
  auto bytes = fixture.make("dependency/Base",
                            "<T:Ljava/lang/Object;>Ljava/lang/Object;");
  Budget budget;
  auto cls =
      parseClassDeclaration(bytes, "fixture.jar!dependency/Base.class", budget);
  EXPECT_TRUE(cls.external);
  EXPECT_EQ(cls.name, "Ldependency/Base;");
  EXPECT_EQ(cls.superclass, "Ljava/lang/Object;");
  EXPECT_EQ(cls.access, (Access{"public", "abstract"}));
  ASSERT_EQ(cls.methods.size(), 1u);
  EXPECT_EQ(cls.methods[0].reference.identity(),
            "Ldependency/Base;->value(I)I");
  EXPECT_TRUE(cls.methods[0].instructions.empty());
  EXPECT_EQ(cls.generic_signature, "<T:Ljava/lang/Object;>Ljava/lang/Object;");
  auto linked = linkClasses({cls}, budget);
  EXPECT_TRUE(linked.at(cls.name).external);
}

TEST(MobileJavaClass, RejectsEveryTruncationAndTrailingBytes) {
  ClassFile fixture;
  auto bytes = fixture.make();
  for (size_t length = 0; length < bytes.size(); ++length) {
    Budget budget;
    EXPECT_THROW(
        parseClassDeclaration(std::string_view(bytes).substr(0, length),
                              "truncated.class", budget),
        Error)
        << length;
  }
  Budget budget;
  EXPECT_THROW(parseClassDeclaration(bytes + std::string(1, '\0'),
                                     "trailing.class", budget),
               Error);
}

TEST(MobileJavaClass, RejectsBadConstantPoolKindsAndUnsupportedVersions) {
  ClassFile fixture;
  auto bytes = fixture.make();
  auto invalid = bytes;
  invalid[10] = 0x7f;
  Budget b1;
  EXPECT_THROW(parseClassDeclaration(invalid, "pool.class", b1), Error);
  invalid = bytes;
  invalid[7] = 62;
  Budget b2;
  EXPECT_THROW(parseClassDeclaration(invalid, "version.class", b2), Error);
  invalid = bytes;
  // this_class must refer to a Class entry, never its underlying UTF-8.
  auto this_index = 10 + fixture.pool.size() + 2;
  invalid[this_index + 1] = 1;
  Budget b3;
  EXPECT_THROW(parseClassDeclaration(invalid, "identity.class", b3), Error);
}

TEST(MobileJavaClass, InputDefinitionsOverrideDependenciesInEitherOrder) {
  ClassFile fixture;
  Budget budget;
  auto dependency = parseClassDeclaration(fixture.make(), "base.class", budget);
  auto input = dependency;
  input.external = false;
  input.methods.clear();
  for (bool dependency_first : {false, true}) {
    Budget b;
    auto linked = linkClasses(dependency_first ? std::vector{dependency, input}
                                               : std::vector{input, dependency},
                              b);
    ASSERT_EQ(linked.size(), 1u);
    EXPECT_FALSE(linked.at(input.name).external);
    EXPECT_TRUE(linked.at(input.name).methods.empty());
  }
  EXPECT_THROW(linkClasses({dependency, dependency}, budget), Error);
  EXPECT_THROW(linkClasses({input, dependency, dependency}, budget), Error);
  EXPECT_THROW(linkClasses({dependency, input, dependency}, budget), Error);
}
} // namespace
