import fixture.GenericBox;
import fixture.GenericOps;
import java.lang.reflect.Constructor;
import java.lang.reflect.GenericArrayType;
import java.lang.reflect.GenericDeclaration;
import java.lang.reflect.Method;
import java.lang.reflect.ParameterizedType;
import java.lang.reflect.Type;
import java.lang.reflect.TypeVariable;
import java.lang.reflect.WildcardType;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

public final class GenericHarness {
    private static final Map<String, String> facts = new TreeMap<String, String>();
    private static final String BOX = "class:fixture.GenericBox";
    private static final String T = "ref(" + BOX + "#T)";

    private static void require(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    private static String declaration(GenericDeclaration value) {
        if (value instanceof Class<?>) return "class:" + ((Class<?>) value).getName();
        if (value instanceof Method) {
            Method method = (Method) value;
            StringBuilder name = new StringBuilder("method:");
            name.append(method.getDeclaringClass().getName()).append('#').append(method.getName()).append('(');
            Class<?>[] parameters = method.getParameterTypes();
            for (int i = 0; i < parameters.length; i++) {
                if (i != 0) name.append(',');
                name.append(parameters[i].getName());
            }
            return name.append(')').toString();
        }
        throw new AssertionError("Unexpected generic declaration " + value);
    }

    private static String types(Type[] values, int depth) {
        StringBuilder result = new StringBuilder("[");
        for (int i = 0; i < values.length; i++) {
            if (i != 0) result.append(',');
            result.append(type(values[i], depth + 1));
        }
        return result.append(']').toString();
    }

    private static String type(Type value, int depth) {
        require(depth <= 32, "Unexpected recursive generic reflection graph");
        if (value instanceof Class<?>) return "class:" + ((Class<?>) value).getName();
        if (value instanceof TypeVariable<?>) {
            TypeVariable<?> variable = (TypeVariable<?>) value;
            // Do not recursively expand bounds: Comparable<N> refers back to N.
            return "ref(" + declaration(variable.getGenericDeclaration()) + "#" + variable.getName() + ")";
        }
        if (value instanceof GenericArrayType) {
            return "array(" + type(((GenericArrayType) value).getGenericComponentType(), depth + 1) + ")";
        }
        if (value instanceof ParameterizedType) {
            ParameterizedType parameterized = (ParameterizedType) value;
            Type owner = parameterized.getOwnerType();
            return "parameterized(" + type(parameterized.getRawType(), depth + 1) + ",owner="
                    + (owner == null ? "null" : type(owner, depth + 1)) + ",args="
                    + types(parameterized.getActualTypeArguments(), depth + 1) + ")";
        }
        if (value instanceof WildcardType) {
            WildcardType wildcard = (WildcardType) value;
            return "wildcard(upper=" + types(wildcard.getUpperBounds(), depth + 1)
                    + ",lower=" + types(wildcard.getLowerBounds(), depth + 1) + ")";
        }
        throw new AssertionError("Unknown reflection Type " + value);
    }

    private static String parameters(TypeVariable<?>[] variables) {
        StringBuilder result = new StringBuilder("[");
        for (int i = 0; i < variables.length; i++) {
            if (i != 0) result.append(',');
            result.append(type(variables[i], 0)).append(" bounds=").append(types(variables[i].getBounds(), 0));
        }
        return result.append(']').toString();
    }

    private static void fact(String key, String actual, String expected) {
        require(actual.equals(expected), key + ": expected " + expected + ", got " + actual);
        require(facts.put(key, actual) == null, "Duplicate reflection key " + key);
        System.out.println("reflection:" + key + "=1");
    }

    private static String list(String argument) {
        return "parameterized(class:java.util.List,owner=null,args=[" + argument + "])";
    }

    private static String methodVariable(String owner, String name, String erasedParameter, String variable) {
        return "ref(method:fixture." + owner + "#" + name + "(" + erasedParameter + ")#" + variable + ")";
    }

    private static void method(Class<?> owner, String name, Class<?>[] erased, String variables,
                               String arguments, String returns) throws Exception {
        Method method = owner.getDeclaredMethod(name, erased);
        require(!method.isBridge() && !method.isSynthetic(), "Unexpected compiler bridge: " + name);
        String actual = parameters(method.getTypeParameters()) + ";args="
                + types(method.getGenericParameterTypes(), 0) + ";return=" + type(method.getGenericReturnType(), 0)
                + ";throws=" + types(method.getGenericExceptionTypes(), 0);
        fact(owner.getSimpleName() + "." + name, actual,
                variables + ";args=" + arguments + ";return=" + returns + ";throws=[]");
    }

    private static void inspect() throws Exception {
        TypeVariable<?>[] listParameters = List.class.getTypeParameters();
        fact("platform.List", "interface=" + List.class.isInterface() + ";declared-here="
                + (listParameters.length == 1 && listParameters[0].getGenericDeclaration() == List.class)
                + ";" + parameters(listParameters),
                "interface=true;declared-here=true;[ref(class:java.util.List#E) bounds=[class:java.lang.Object]]");
        TypeVariable<?>[] comparableParameters = Comparable.class.getTypeParameters();
        fact("platform.Comparable", "interface=" + Comparable.class.isInterface() + ";declared-here="
                + (comparableParameters.length == 1 && comparableParameters[0].getGenericDeclaration() == Comparable.class)
                + ";" + parameters(comparableParameters),
                "interface=true;declared-here=true;[ref(class:java.lang.Comparable#T) bounds=[class:java.lang.Object]]");
        for (Class<?> owner : new Class<?>[] {GenericBox.class, GenericOps.class}) {
            require(!owner.isLocalClass() && !owner.isMemberClass() && !owner.isAnonymousClass(), "Unexpected nested class");
            String variables = owner == GenericBox.class ? "[" + T + " bounds=[class:java.lang.Object]]" : "[]";
            fact(owner.getSimpleName() + ".class", parameters(owner.getTypeParameters())
                    + ";super=" + type(owner.getGenericSuperclass(), 0) + ";interfaces="
                    + types(owner.getGenericInterfaces(), 0), variables + ";super=class:java.lang.Object;interfaces=[]");
            require(owner.getDeclaredConstructors().length == 1, "Unexpected constructor inventory");
        }
        require(GenericBox.class.getDeclaredFields().length == 5 && GenericOps.class.getDeclaredFields().length == 0,
                "Unexpected field inventory");
        String[] names = {"value", "values", "upper", "lower", "any"};
        String[] expected = {T, "array(" + T + ")", list("wildcard(upper=[" + T + "],lower=[])"),
                list("wildcard(upper=[class:java.lang.Object],lower=[" + T + "])"),
                list("wildcard(upper=[class:java.lang.Object],lower=[])")};
        for (int i = 0; i < names.length; i++) {
            fact("GenericBox.field." + names[i], type(GenericBox.class.getField(names[i]).getGenericType(), 0), expected[i]);
        }
        Constructor<?> box = GenericBox.class.getConstructor(Object.class);
        fact("GenericBox.constructor", parameters(box.getTypeParameters()) + ";args="
                + types(box.getGenericParameterTypes(), 0), "[];args=[" + T + "]");
        Constructor<?> ops = GenericOps.class.getConstructor();
        fact("GenericOps.constructor", parameters(ops.getTypeParameters()) + ";args="
                + types(ops.getGenericParameterTypes(), 0), "[];args=[]");
        method(GenericBox.class, "get", new Class<?>[] {}, "[]", "[]", T);
        method(GenericBox.class, "exchange", new Class<?>[] {Object.class}, "[]", "[" + T + "]", T);
        method(GenericBox.class, "arrayIdentity", new Class<?>[] {Object[].class}, "[]", "[array(" + T + ")]", "array(" + T + ")");
        method(GenericBox.class, "first", new Class<?>[] {Object[].class, int.class}, "[]", "[array(" + T + "),class:int]", T);
        method(GenericBox.class, "arrayExchange", new Class<?>[] {Object[].class, int.class, Object.class},
                "[]", "[array(" + T + "),class:int," + T + "]", T);
        String shadow = methodVariable("GenericBox", "shadow", "java.lang.Number", "T");
        method(GenericBox.class, "shadow", new Class<?>[] {Number.class}, "[" + shadow + " bounds=[class:java.lang.Number]]",
                "[" + shadow + "]", shadow);
        String identity = methodVariable("GenericOps", "identity", "java.lang.Object", "U");
        method(GenericOps.class, "identity", new Class<?>[] {Object.class}, "[" + identity + " bounds=[class:java.lang.Object]]",
                "[" + identity + "]", identity);
        String intersection = methodVariable("GenericOps", "intersection", "java.lang.Number", "N");
        method(GenericOps.class, "intersection", new Class<?>[] {Number.class}, "[" + intersection
                + " bounds=[class:java.lang.Number,parameterized(class:java.lang.Comparable,owner=null,args=[" + intersection + "])]]",
                "[" + intersection + "]", intersection);
        String iface = methodVariable("GenericOps", "interfaceOnly", "java.lang.CharSequence", "I");
        method(GenericOps.class, "interfaceOnly", new Class<?>[] {CharSequence.class}, "[" + iface + " bounds=[class:java.lang.CharSequence]]",
                "[" + iface + "]", iface);
        String lower = methodVariable("GenericOps", "lowerIdentity", "java.util.List", "U");
        String lowerType = list("wildcard(upper=[class:java.lang.Object],lower=[" + lower + "])");
        method(GenericOps.class, "lowerIdentity", new Class<?>[] {List.class}, "[" + lower + " bounds=[class:java.lang.Object]]",
                "[" + lowerType + "]", lowerType);
        method(GenericOps.class, "scalar", new Class<?>[] {int.class}, "[]", "[class:int]", "class:int");
    }

    private static void value(String key, boolean success) {
        require(success, "Behavior failed: " + key);
        System.out.println(key + "=1");
    }

    @SuppressWarnings({"rawtypes", "unchecked"})
    private static void behavior() {
        Object[] sentinels = {null, new Object(), new String("first"), Integer.valueOf(9001)};
        for (int i = 0; i < sentinels.length; i++) {
            GenericBox<Object> box = new GenericBox<Object>(sentinels[i]);
            value("constructor:" + i, box.get() == sentinels[i]);
            value("identity:" + i, GenericOps.identity(sentinels[i]) == sentinels[i]);
            for (int j = 0; j < sentinels.length; j++) {
                Object previous = box.value;
                value("exchange-old:" + i + ":" + j, box.exchange(sentinels[j]) == previous);
                value("exchange-new:" + i + ":" + j, box.value == sentinels[j] && box.get() == sentinels[j]);
            }
            Object[] array = sentinels.clone();
            box.values = array;
            value("array-identity:" + i, box.arrayIdentity(array) == array && box.values == array);
            value("array-null:" + i, box.arrayIdentity(null) == null);
            value("array-read:" + i, box.first(array, i) == sentinels[i]);
            Object next = sentinels[(i + 1) % sentinels.length];
            value("array-old:" + i, box.arrayExchange(array, i, next) == sentinels[i]);
            value("array-new:" + i, array[i] == next && box.values[i] == next);
        }
        GenericBox<String> strings = new GenericBox<String>("before");
        List<String> upper = Arrays.asList("a", "b");
        List<Object> lower = new ArrayList<Object>();
        List<Integer> any = Arrays.asList(Integer.valueOf(3));
        strings.upper = upper;
        strings.lower = lower;
        strings.any = any;
        value("wildcard-fields", strings.upper == upper && strings.lower == lower && strings.any == any);
        value("wildcard-identity", GenericOps.<String>lowerIdentity(lower) == lower);
        value("wildcard-null", GenericOps.<String>lowerIdentity(null) == null);
        Integer integer = Integer.valueOf(9011);
        Long wide = Long.valueOf(9223372036854775000L);
        value("intersection-integer", GenericOps.intersection(integer) == integer);
        value("intersection-long", GenericOps.intersection(wide) == wide);
        value("intersection-null", GenericOps.<Integer>intersection(null) == null);
        String text = new String("interface");
        StringBuilder builder = new StringBuilder("builder");
        value("interface-string", GenericOps.interfaceOnly(text) == text);
        value("interface-builder", GenericOps.interfaceOnly(builder) == builder);
        value("interface-null", GenericOps.<String>interfaceOnly(null) == null);
        value("shadow-integer", strings.shadow(integer) == integer);
        value("shadow-long", strings.shadow(wide) == wide);
        value("shadow-null", strings.<Integer>shadow(null) == null);
        value("ops-constructor", new GenericOps().getClass() == GenericOps.class);
        int[] scalars = {Integer.MIN_VALUE, -100, -1, 0, 1, 100, Integer.MAX_VALUE};
        for (int i = 0; i < scalars.length; i++) System.out.println("scalar:" + i + "=" + GenericOps.scalar(scalars[i]));
        int exceptions = 0;
        try { strings.first(null, 0); } catch (NullPointerException expected) { exceptions |= 1; }
        try { strings.first(new String[0], 0); } catch (ArrayIndexOutOfBoundsException expected) { exceptions |= 2; }
        String[] storage = {"unchanged"};
        try { ((GenericBox) strings).arrayExchange(storage, 0, integer); }
        catch (ArrayStoreException expected) { exceptions |= 4; }
        require(exceptions == 7 && storage[0].equals("unchanged"), "Array exception/store ordering changed");
        System.out.println("array-exceptions=" + exceptions);
    }

    public static void main(String[] args) throws Exception {
        require(args.length == 1, "Expected reflection evidence path");
        inspect();
        List<String> lines = new ArrayList<String>();
        for (Map.Entry<String, String> row : facts.entrySet()) lines.add(row.getKey() + "\t" + row.getValue());
        Files.write(Paths.get(args[0]), lines, StandardCharsets.UTF_8);
        behavior();
    }
}
