package fixture;
import java.lang.reflect.*;

public final class PrimitiveBridgeCopiesHarness {
    private static int values, identities, states, failures;
    private interface Call {
        Object run(Object marker, Object input, int number, float single, long integer, double decimal) throws Throwable;
    }
    private static final class Leaf extends Core<CharSequence> {
        int scalar(Object marker, CharSequence input, int number, float single, long integer, double decimal) {
            calls += 2; return super.scalar(marker, input, number, single, integer, decimal);
        }
        float single(Object marker, CharSequence input, int number, float single, long integer, double decimal) {
            calls += 2; return super.single(marker, input, number, single, integer, decimal);
        }
        long wide(Object marker, CharSequence input, int number, float single, long integer, double decimal) {
            calls += 2; return super.wide(marker, input, number, single, integer, decimal);
        }
        double decimal(Object marker, CharSequence input, int number, float single, long integer, double decimal) {
            calls += 2; return super.decimal(marker, input, number, single, integer, decimal);
        }
    }
    private static void same(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("original mixed bridge identity changed");
        identities++;
    }
    private static Method entry(String name, boolean bridge) throws Exception {
        for (Method method : Core.class.getDeclaredMethods())
            if (method.getName().equals(name) && method.isBridge() == bridge) return method;
        throw new AssertionError("original package bridge missing");
    }
    private static Object invoke(Method method, Object receiver, Object marker, Object input,
                                 int number, float single, long integer, double decimal) throws Throwable {
        try { return method.invoke(receiver, marker, input, number, single, integer, decimal); }
        catch (InvocationTargetException error) { throw error.getCause(); }
    }
    private static Object direct(int index, Core<CharSequence> receiver, Object marker, CharSequence input,
                                 int number, float single, long integer, double decimal) {
        switch (index) {
            case 0: return receiver.scalar(marker, input, number, single, integer, decimal);
            case 1: return receiver.single(marker, input, number, single, integer, decimal);
            case 2: return receiver.wide(marker, input, number, single, integer, decimal);
            default: return receiver.decimal(marker, input, number, single, integer, decimal);
        }
    }
    private static <T> Object inherited(int index, Parent<T> receiver, Object marker, T input,
                                       int number, float single, long integer, double decimal) {
        switch (index) {
            case 0: return receiver.scalar(marker, input, number, single, integer, decimal);
            case 1: return receiver.single(marker, input, number, single, integer, decimal);
            case 2: return receiver.wide(marker, input, number, single, integer, decimal);
            default: return receiver.decimal(marker, input, number, single, integer, decimal);
        }
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    private static Call[] paths(int index, String name, Core<CharSequence> receiver) throws Exception {
        Method typed = entry(name, false), bridge = entry(name, true);
        Method parent = Parent.class.getDeclaredMethod(name, Object.class, Object.class,
                                                      int.class, float.class, long.class, double.class);
        Parent<CharSequence> required = receiver;
        Parent raw = receiver;
        return new Call[] {
            (m, t, i, f, j, d) -> direct(index, receiver, m, (CharSequence) t, i, f, j, d),
            (m, t, i, f, j, d) -> inherited(index, required, m, (CharSequence) t, i, f, j, d),
            (m, t, i, f, j, d) -> inherited(index, raw, m, t, i, f, j, d),
            (m, t, i, f, j, d) -> invoke(typed, receiver, m, t, i, f, j, d),
            (m, t, i, f, j, d) -> invoke(bridge, receiver, m, t, i, f, j, d),
            (m, t, i, f, j, d) -> invoke(parent, receiver, m, t, i, f, j, d)
        };
    }
    private static void reset(Core<?> receiver, RuntimeException failure) {
        receiver.calls = 0; receiver.seenMarker = null; receiver.seenInput = null;
        receiver.seenNumber = 0; receiver.seenSingle = 0; receiver.seenInteger = 0; receiver.seenDecimal = 0;
        receiver.failure = failure;
    }
    private static void state(Core<?> receiver, int calls, Object marker, CharSequence input,
                              int number, float single, long integer, double decimal, RuntimeException failure) {
        if (receiver.calls != calls || receiver.seenNumber != number || receiver.seenInteger != integer
                || Float.floatToRawIntBits(receiver.seenSingle) != Float.floatToRawIntBits(single)
                || Double.doubleToRawLongBits(receiver.seenDecimal) != Double.doubleToRawLongBits(decimal))
            throw new AssertionError("original scalar or wide parameter bits changed");
        same(receiver.seenMarker, marker); same(receiver.seenInput, input); same(receiver.failure, failure);
        states++;
    }
    private static void result(int index, Object actual, int number, float single, long integer, double decimal) {
        boolean equal = index == 0 ? actual instanceof Integer && ((Integer) actual).intValue() == number
            : index == 1 ? actual instanceof Float && Float.floatToRawIntBits((Float) actual) == Float.floatToRawIntBits(single)
            : index == 2 ? actual instanceof Long && ((Long) actual).longValue() == integer
            : actual instanceof Double && Double.doubleToRawLongBits((Double) actual) == Double.doubleToRawLongBits(decimal);
        if (!equal) throw new AssertionError("original primitive bridge return bits changed");
        values++;
    }
    public static void main(String[] args) throws Throwable {
        String[] names = {"scalar", "single", "wide", "decimal"};
        Class<?>[] returns = {int.class, float.class, long.class, double.class};
        for (int index = 0; index < names.length; index++) {
            Method typed = entry(names[index], false), bridge = entry(names[index], true);
            if (typed.getReturnType() != returns[index] || bridge.getReturnType() != returns[index]
                    || !typed.getGenericParameterTypes()[1].getTypeName().equals("G")
                    || bridge.getParameterTypes()[1] != Object.class || !bridge.isBridge() || !bridge.isSynthetic()
                    || (typed.getModifiers() & (Modifier.PUBLIC | Modifier.PROTECTED | Modifier.PRIVATE)) != 0
                    || (bridge.getModifiers() & (Modifier.PUBLIC | Modifier.PROTECTED | Modifier.PRIVATE)) != 0
                    || typed.getExceptionTypes().length != 0 || bridge.getExceptionTypes().length != 0)
                throw new AssertionError("original package visibility, generic scope or primitive bridge changed");
        }
        int[] numbers = {Integer.MIN_VALUE, -1, 0, Integer.MAX_VALUE, 123};
        int[] singles = {0, 0x80000000, 0x7f800000, 0xff800000, 0x7fc01234};
        long[] integers = {Long.MIN_VALUE, Long.MAX_VALUE, -1, 0, 0x1234567887654321L};
        long[] decimals = {0, 0x8000000000000000L, 0x7ff0000000000000L, 0xfff0000000000000L, 0x7ff8123456789abcL};
        for (Core<CharSequence> receiver : new Core[] {new Core<CharSequence>(), new Leaf()}) {
            int delta = receiver instanceof Leaf ? 3 : 1;
            for (int index = 0; index < names.length; index++) {
                Call[] routes = paths(index, names[index], receiver), absent = paths(index, names[index], null);
                for (int route = 0; route < routes.length; route++) {
                    for (CharSequence input : new CharSequence[] {null, new String("owned input"), new StringBuilder("owned alias")})
                        for (int sample = 0; sample < numbers.length; sample++) {
                            Object[] marker = {input}; int i = numbers[sample]; float f = Float.intBitsToFloat(singles[sample]);
                            long j = integers[sample]; double d = Double.longBitsToDouble(decimals[sample]);
                            reset(receiver, null);
                            result(index, routes[route].run(marker, input, i, f, j, d), i, f, j, d);
                            state(receiver, delta, marker, input, i, f, j, d, null);
                            marker[0] = receiver; same(((Object[]) receiver.seenMarker)[0], receiver);
                        }
                    for (int mode = 0; mode < 4; mode++) {
                        RuntimeException failure = mode == 0 || mode == 3 ? new RuntimeException("owned failure") : null;
                        CharSequence input = new StringBuilder("owned failure input"); Object marker = new Object();
                        Object argument = mode == 1 || mode == 3 ? new Object() : input;
                        reset(receiver, failure);
                        try {
                            (mode == 2 ? absent[route] : routes[route]).run(marker, argument, 17, -0.0f, Long.MIN_VALUE, -0.0d);
                            throw new AssertionError("original mixed bridge failure missing");
                        } catch (Throwable error) {
                            if (mode == 0) same(error, failure);
                            else same(error.getClass(), mode == 2 ? NullPointerException.class
                                      : route == 3 ? IllegalArgumentException.class : ClassCastException.class);
                            failures++;
                        }
                        state(receiver, mode == 0 ? delta : 0, mode == 0 ? marker : null,
                              mode == 0 ? input : null, mode == 0 ? 17 : 0, mode == 0 ? -0.0f : 0.0f,
                              mode == 0 ? Long.MIN_VALUE : 0, mode == 0 ? -0.0d : 0.0d, failure);
                    }
                }
            }
        }
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("scalar-wide-bits-and-origin=retained,complete-class-formal=retained,original-package-bridges=retained");
    }
}
