import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class PrivateSeededPrimitiveHarness {
    static int mode, shape, attempt, calls;
    static Subject receiver;
    static Callback callback;
    static Object value;
    static long wanted;
    static String events;
    static Throwable failure;
    static final String[] names = {"Z", "I", "J", "F", "D"};
    static final Class<?>[] types = {boolean.class, int.class, long.class, float.class, double.class};
    static void check(boolean value) { if (!value) throw new AssertionError("owned private seeded primitive"); }
    static <E extends Throwable> void raise(Throwable value) throws E { throw (E)value; }
    static final class Checked extends Exception {}
    static void fields(Subject target, int salt) {
        boolean odd = (salt & 1) != 0;
        target.z = odd; target.i = odd ? 0x76543210 : Integer.MIN_VALUE;
        target.j = odd ? 0x8123456789abcdefL : Long.MIN_VALUE;
        target.f = Float.intBitsToFloat(odd ? 0x7fc01234 : 0x80000000);
        target.d = Double.longBitsToDouble(odd ? 0x7ff8000000001234L : 0x8000000000000000L);
    }
    static long bits(Subject target) {
        switch (shape) {
        case 0: return target.z ? 1 : 0;
        case 1: return target.i;
        case 2: return target.j;
        case 3: return Float.floatToRawIntBits(target.f);
        default: return Double.doubleToRawLongBits(target.d);
        }
    }
    static long bits(Object result) {
        switch (shape) {
        case 0: return ((Boolean)result).booleanValue() ? 1 : 0;
        case 1: return ((Integer)result).intValue();
        case 2: return ((Long)result).longValue();
        case 3: return Float.floatToRawIntBits(((Float)result).floatValue());
        default: return Double.doubleToRawLongBits(((Double)result).doubleValue());
        }
    }
    static final class Probe implements Callback<Object> {
        public void accept(Object actual) {
            calls++; events += "C,"; check(actual == value);
            if (mode == 5 || mode == 6) PrivateSeededPrimitiveHarness.<RuntimeException>raise(failure);
            if (mode == 9) fields(receiver, attempt + 1);
        }
    }
    static final class Observer extends Trace {
        public void observe(int phase, Subject target, Callback<?> action, Object actual) {
            events += phase + ",";
            check(target == receiver && action == callback && actual == value);
            check(target.state == (phase == 1 ? 7 : 8));
            if (phase == 2) {
                wanted = bits(target);
                if (mode == 11) fields(target, attempt + 1);
            }
            if (phase == (mode == 4 ? 1 : mode == 7 || mode == 10 ? 2 : -1))
                PrivateSeededPrimitiveHarness.<RuntimeException>raise(failure);
        }
    }
    static Object call(int route) throws Throwable {
        if (route == 0) return Core.call(receiver, callback, value, shape);
        if (route == 1 || route == 3) {
            try {
                return route == 1 ? Core.class.getMethod("call", Subject.class, Callback.class, Object.class, int.class)
                    .invoke(null, receiver, callback, value, shape)
                    : Subject.class.getMethod("run" + names[shape], Callback.class, Object.class).invoke(receiver, callback, value);
            } catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return MethodHandles.publicLookup().findStatic(Core.class, "call",
            MethodType.methodType(Object.class, Subject.class, Callback.class, Object.class, int.class))
            .invokeWithArguments(receiver, callback, value, shape);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]); shape = Integer.parseInt(args[1]); mode = Integer.parseInt(args[2]);
        Method method = Subject.class.getDeclaredMethod("select" + names[shape], Callback.class, Object.class);
        TypeVariable<?>[] formals = method.getTypeParameters();
        check(Modifier.isPrivate(method.getModifiers()) && Modifier.isFinal(method.getModifiers()) && !Modifier.isStatic(method.getModifiers()));
        check(formals.length == 1 && formals[0].getBounds().length == 1 && formals[0].getBounds()[0] == Object.class);
        Type slot = ((ParameterizedType)method.getGenericParameterTypes()[0]).getActualTypeArguments()[0];
        check(slot instanceof WildcardType && ((WildcardType)slot).getLowerBounds()[0].equals(formals[0]));
        check(method.getGenericParameterTypes()[1].equals(formals[0]) && method.getReturnType() == types[shape]);
        for (attempt = 0; attempt < 2; attempt++) {
            Subject original = new Subject(); original.state = 7; fields(original, attempt);
            receiver = mode == 8 ? null : original; callback = mode == 1 ? null : new Probe();
            value = mode == 2 ? null : mode == 3 ? new Object() : new String("owned seed");
            events = ""; calls = 0; Trace.hook = new Observer();
            failure = mode == 4 || mode == 7 ? new Checked() : mode == 6 || mode == 10 ? new LinkageError("owned error") : new RuntimeException("owned failure");
            Object result = null; Throwable caught = null;
            try { result = call(route); } catch (Throwable problem) { caught = problem; }
            boolean entered = mode != 8;
            boolean called = entered && mode != 1 && mode != 4;
            boolean stored = called && mode != 5 && mode != 6;
            String expected = entered ? "1," : "";
            if (called) expected += "C,";
            if (stored) expected += "2,";
            check(events.equals(expected) && calls == (called ? 1 : 0) && original.state == (stored ? 8 : 7));
            if (mode == 4 || mode == 5 || mode == 6 || mode == 7 || mode == 10) check(caught == failure);
            else if (mode == 1 || mode == 8) check(caught instanceof NullPointerException);
            else check(caught == null && bits(result) == wanted);
        }
        System.out.println("attempts=2,private-seeded-primitive=matched");
    }
}
