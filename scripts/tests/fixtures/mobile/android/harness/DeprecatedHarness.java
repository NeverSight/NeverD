import fixture.DeprecatedBase;
import fixture.PlainChild;
import java.lang.annotation.Annotation;
import java.lang.reflect.AnnotatedElement;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.Map;
import java.util.TreeMap;

public final class DeprecatedHarness {
    private static final Map<String, String> facts = new TreeMap<String, String>();

    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    private static void value(String key, int actual, int expected) {
        require(actual == expected, key + ": expected " + expected + ", got " + actual);
        System.out.println(key + "=" + actual);
    }

    private static void marker(String key, AnnotatedElement element, boolean expected) {
        Annotation[] declared = element.getDeclaredAnnotations();
        Annotation[] visible = element.getAnnotations();
        require(declared.length == (expected ? 1 : 0) && visible.length == declared.length,
                key + ": annotation inventory changed");
        require(element.isAnnotationPresent(Deprecated.class) == expected
                && (element.getAnnotation(Deprecated.class) != null) == expected
                && (element.getDeclaredAnnotation(Deprecated.class) != null) == expected,
                key + ": runtime marker changed");
        if (expected) {
            require(declared[0].annotationType() == Deprecated.class
                    && visible[0].annotationType() == Deprecated.class,
                    key + ": wrong annotation declaration");
        }
        String text = "declared=" + (expected ? declared[0].annotationType().getName() : "")
                + ";present=" + element.isAnnotationPresent(Deprecated.class);
        require(facts.put(key, text) == null, "Duplicate reflection identity " + key);
        value("reflection:" + key, 1, 1);
    }

    private static void inspect() throws Exception {
        require(DeprecatedBase.class.getSuperclass() == Object.class
                && PlainChild.class.getSuperclass() == DeprecatedBase.class,
                "Superclass identity changed");
        require(DeprecatedBase.class.getDeclaredFields().length == 3
                && PlainChild.class.getDeclaredFields().length == 1,
                "Field inventory changed");
        require(DeprecatedBase.class.getDeclaredConstructors().length == 2
                && PlainChild.class.getDeclaredConstructors().length == 1,
                "Constructor inventory changed");
        marker("DeprecatedBase.class", DeprecatedBase.class, true);
        marker("PlainChild.class", PlainChild.class, false);
        marker("DeprecatedBase.field.legacy", DeprecatedBase.class.getDeclaredField("legacy"), true);
        marker("DeprecatedBase.field.current", DeprecatedBase.class.getDeclaredField("current"), false);
        marker("DeprecatedBase.field.constructorCalls", DeprecatedBase.class.getDeclaredField("constructorCalls"), false);
        marker("PlainChild.field.own", PlainChild.class.getDeclaredField("own"), false);
        Constructor<?> deprecated = DeprecatedBase.class.getConstructor();
        Constructor<?> plain = DeprecatedBase.class.getConstructor(int.class);
        marker("DeprecatedBase.constructor.empty", deprecated, true);
        marker("DeprecatedBase.constructor.int", plain, false);
        marker("PlainChild.constructor.int", PlainChild.class.getConstructor(int.class), false);
        marker("DeprecatedBase.method.oldAdd", DeprecatedBase.class.getDeclaredMethod("oldAdd", int.class), true);
        marker("DeprecatedBase.method.combine", DeprecatedBase.class.getDeclaredMethod("combine", int.class), false);
        marker("PlainChild.method.childValue", PlainChild.class.getDeclaredMethod("childValue", int.class), false);
        Field inheritedField = PlainChild.class.getField("legacy");
        Method inheritedMethod = PlainChild.class.getMethod("oldAdd", int.class);
        require(inheritedField.getDeclaringClass() == DeprecatedBase.class
                && inheritedMethod.getDeclaringClass() == DeprecatedBase.class
                && inheritedField.isAnnotationPresent(Deprecated.class)
                && inheritedMethod.isAnnotationPresent(Deprecated.class),
                "Inherited member lookup lost its original declaration marker");
        String lookup = "field=" + inheritedField.getDeclaringClass().getName()
                + ";method=" + inheritedMethod.getDeclaringClass().getName()
                + ";class-present=" + PlainChild.class.isAnnotationPresent(Deprecated.class);
        require(lookup.equals("field=fixture.DeprecatedBase;method=fixture.DeprecatedBase;class-present=false"),
                "Deprecated must not be inherited as a class annotation");
        require(facts.put("PlainChild.inherited", lookup) == null, "Duplicate inherited fact");
        value("reflection:PlainChild.inherited", 1, 1);
    }

    private static void behavior() {
        DeprecatedBase.constructorCalls = 0;
        int[] inputs = {-100, -1, 0, 1, 7, 100};
        for (int i = 0; i < inputs.length; i++) {
            int input = inputs[i];
            int before = DeprecatedBase.constructorCalls;
            DeprecatedBase old = new DeprecatedBase();
            value("deprecated:" + i + ":constructor-delta", DeprecatedBase.constructorCalls - before, 1);
            value("deprecated:" + i + ":old", old.oldAdd(input), 7);
            value("deprecated:" + i + ":new", old.legacy, 7 + input);
            value("deprecated:" + i + ":combine", old.combine(input), 33 - input);
            before = DeprecatedBase.constructorCalls;
            DeprecatedBase plain = new DeprecatedBase(input);
            value("plain:" + i + ":constructor-delta", DeprecatedBase.constructorCalls - before, 1);
            value("plain:" + i + ":old", plain.oldAdd(-2), input);
            value("plain:" + i + ":new", plain.legacy, input - 2);
            value("plain:" + i + ":combine", plain.combine(input), 2 * input + 9);
            before = DeprecatedBase.constructorCalls;
            PlainChild child = new PlainChild(input);
            value("child:" + i + ":constructor-delta", DeprecatedBase.constructorCalls - before, 1);
            value("child:" + i + ":old", child.oldAdd(2), input);
            value("child:" + i + ":new", child.legacy, input + 2);
            value("child:" + i + ":combine", child.combine(input), 2 * input + 9);
            value("child:" + i + ":own", child.childValue(input), 5 * input - 2);
        }
        value("final:constructor-count", DeprecatedBase.constructorCalls, 18);
    }

    public static void main(String[] args) throws Exception {
        require(args.length == 1, "Expected reflection evidence path");
        inspect();
        StringBuilder text = new StringBuilder();
        for (Map.Entry<String, String> entry : facts.entrySet()) {
            text.append(entry.getKey()).append('\t').append(entry.getValue()).append('\n');
        }
        Files.write(Paths.get(args[0]), text.toString().getBytes(StandardCharsets.UTF_8));
        behavior();
    }
}
