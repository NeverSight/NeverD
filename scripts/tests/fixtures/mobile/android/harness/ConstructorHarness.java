import fixture.PlainBase;
import fixture.SuperChild;
import fixture.ThisChoice;
import java.lang.reflect.Constructor;
import java.lang.reflect.Type;
import java.lang.reflect.TypeVariable;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.Arrays;
import java.util.Map;
import java.util.TreeMap;

public final class ConstructorHarness {
    private static final Map<String, String> facts = new TreeMap<String, String>();

    private static void require(boolean success, String message) {
        if (!success) throw new AssertionError(message);
    }

    private static void value(String key, int actual, int expected) {
        require(actual == expected, key + ": expected " + expected + ", got " + actual);
        System.out.println(key + "=" + actual);
    }

    private static void fact(String key, String actual, String expected) {
        require(actual.equals(expected), key + ": expected " + expected + ", got " + actual);
        require(facts.put(key, actual) == null, "Duplicate reflection key " + key);
        value("reflection:" + key, 1, 1);
    }

    private static void inspect() throws Exception {
        String[] platformNames = {"java.lang.Object", "java.lang.Number", "java.lang.String",
                "java.lang.Exception", "java.lang.Throwable", "java.lang.CharSequence",
                "java.io.Serializable", "java.lang.Comparable", "java.util.List"};
        boolean[] platformInterfaces = {false, false, false, false, false, true, true, true, true};
        for (int i = 0; i < platformNames.length; i++) {
            Class<?> declaration = Class.forName(platformNames[i]);
            fact("KIND." + platformNames[i], declaration.getName() + ";interface=" + declaration.isInterface(),
                    platformNames[i] + ";interface=" + platformInterfaces[i]);
        }
        for (Class<?> owner : new Class<?>[] {PlainBase.class, SuperChild.class, ThisChoice.class}) {
            Constructor<?>[] constructors = owner.getDeclaredConstructors();
            int count = owner == PlainBase.class ? 2 : owner == SuperChild.class ? 1 : 3;
            require(constructors.length == count, "Unexpected constructor inventory for " + owner);
            for (Constructor<?> constructor : constructors) {
                require(!constructor.isSynthetic() && constructor.getModifiers() == 1,
                        "Unexpected constructor access for " + owner);
                require(constructor.getGenericExceptionTypes().length == 0, "Unexpected constructor throws");
            }
            fact(owner.getSimpleName() + ".class", "constructors=" + constructors.length
                    + ";super=" + owner.getSuperclass().getName() + ";formals=" + owner.getTypeParameters().length,
                    "constructors=" + count + ";super="
                    + (owner == SuperChild.class ? "fixture.PlainBase" : "java.lang.Object") + ";formals=0");
            if (owner != SuperChild.class) {
                for (Class<?> argument : new Class<?>[] {Object.class, CharSequence.class}) {
                    Constructor<?> constructor = owner.getConstructor(argument);
                    require(constructor.getTypeParameters().length == 0
                            && Arrays.equals(constructor.getGenericParameterTypes(), new Type[] {argument}),
                            "Plain overload acquired a generic signature");
                }
            }
        }
        for (Class<?> owner : new Class<?>[] {SuperChild.class, ThisChoice.class}) {
            Constructor<?> constructor = owner.getConstructor(Object.class, int.class);
            TypeVariable<?>[] formals = constructor.getTypeParameters();
            require(formals.length == 1, "Expected one constructor type variable");
            TypeVariable<?> formal = formals[0];
            Type[] bounds = formal.getBounds();
            require(bounds.length == 2 && bounds[0] == Object.class && bounds[1] == CharSequence.class,
                    "Constructor intersection bounds changed");
            require(formal.getGenericDeclaration().equals(constructor), "Constructor type variable owner changed");
            require(Arrays.equals(constructor.getGenericParameterTypes(), new Type[] {formal, int.class}),
                    "Constructor generic parameter types changed");
            fact(owner.getSimpleName() + ".generic", constructor.getDeclaringClass().getName()
                    + "#" + formal.getName() + ";bounds=" + bounds[0].getTypeName() + "," + bounds[1].getTypeName()
                    + ";erased=" + constructor.getParameterTypes()[0].getName() + ",int",
                    owner.getName() + "#T;bounds=java.lang.Object,java.lang.CharSequence;erased=java.lang.Object,int");
        }
    }

    private static void behavior() {
        PlainBase.objectCalls = 0;
        PlainBase.sequenceCalls = 0;
        SuperChild.bodyCalls = 0;
        ThisChoice.objectCalls = 0;
        ThisChoice.sequenceCalls = 0;
        ThisChoice.delegatingCalls = 0;
        CharSequence[] inputs = {null, "text", new StringBuilder("text")};
        for (int i = 0; i < inputs.length; i++) {
            for (int choice = 0; choice < 2; choice++) {
                String key = "base-direct:" + choice + ":" + i;
                int objects = PlainBase.objectCalls;
                int sequences = PlainBase.sequenceCalls;
                PlainBase base = choice == 0 ? new PlainBase((Object) inputs[i]) : new PlainBase(inputs[i]);
                value(key + ":tag", base.tag, choice == 0 ? 101 : 202);
                value(key + ":object-count", PlainBase.objectCalls - objects, choice == 0 ? 1 : 0);
                value(key + ":sequence-count", PlainBase.sequenceCalls - sequences, choice == 0 ? 0 : 1);
                value(key + ":received", base.received == inputs[i] ? 1 : 0, 1);
                key = "this-direct:" + choice + ":" + i;
                objects = ThisChoice.objectCalls;
                sequences = ThisChoice.sequenceCalls;
                ThisChoice direct = choice == 0 ? new ThisChoice((Object) inputs[i]) : new ThisChoice(inputs[i]);
                value(key + ":tag", direct.tag, choice == 0 ? 303 : 404);
                value(key + ":object-count", ThisChoice.objectCalls - objects, choice == 0 ? 1 : 0);
                value(key + ":sequence-count", ThisChoice.sequenceCalls - sequences, choice == 0 ? 0 : 1);
                value(key + ":received", direct.received == inputs[i] ? 1 : 0, 1);
            }
        }
        int[] markers = {-7, 19};
        for (int i = 0; i < inputs.length; i++) {
            for (int j = 0; j < markers.length; j++) {
                String key = "super:" + i + ":" + j;
                int objects = PlainBase.objectCalls;
                int sequences = PlainBase.sequenceCalls;
                int bodies = SuperChild.bodyCalls;
                SuperChild child = new SuperChild(inputs[i], markers[j]);
                value(key + ":tag", child.tag, 101);
                value(key + ":object-count", PlainBase.objectCalls - objects, 1);
                value(key + ":sequence-count", PlainBase.sequenceCalls - sequences, 0);
                value(key + ":body-count", SuperChild.bodyCalls - bodies, 1);
                value(key + ":marker", child.marker, markers[j]);
                value(key + ":received", child.received == inputs[i] ? 1 : 0, 1);
                key = "this:" + i + ":" + j;
                objects = ThisChoice.objectCalls;
                sequences = ThisChoice.sequenceCalls;
                bodies = ThisChoice.delegatingCalls;
                ThisChoice delegated = new ThisChoice(inputs[i], markers[j]);
                value(key + ":tag", delegated.tag, 303);
                value(key + ":object-count", ThisChoice.objectCalls - objects, 1);
                value(key + ":sequence-count", ThisChoice.sequenceCalls - sequences, 0);
                value(key + ":body-count", ThisChoice.delegatingCalls - bodies, 1);
                value(key + ":marker", delegated.marker, markers[j]);
                value(key + ":received", delegated.received == inputs[i] ? 1 : 0, 1);
            }
        }
        value("final:base-object", PlainBase.objectCalls, 9);
        value("final:base-sequence", PlainBase.sequenceCalls, 3);
        value("final:super-body", SuperChild.bodyCalls, 6);
        value("final:this-object", ThisChoice.objectCalls, 9);
        value("final:this-sequence", ThisChoice.sequenceCalls, 3);
        value("final:this-body", ThisChoice.delegatingCalls, 6);
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
