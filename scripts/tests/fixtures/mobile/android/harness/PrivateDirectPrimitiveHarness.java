import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class PrivateDirectPrimitiveHarness {
    static int mode, shape, attempt;
    static Subject receiver;
    static Object value, initial, alternative;
    static long wanted;
    static String events;
    static Throwable failure;
    static final String[] names = {"Z", "B", "C", "S", "I", "J", "F", "D"};
    static final Class<?>[] types = {boolean.class, byte.class, char.class, short.class, int.class, long.class, float.class, double.class};
    static void check(boolean value) { if (!value) throw new AssertionError("owned private direct primitive"); }
    static <E extends Throwable> void raise(Throwable value) throws E { throw (E)value; }
    static final class Checked extends Exception {}
    static void fields(Subject target, int salt) {
        boolean odd = (salt & 1) != 0;
        target.z = odd; target.b = odd ? (byte)0x89 : Byte.MIN_VALUE;
        target.c = odd ? (char)0xfedc : (char)0; target.s = odd ? (short)0x8765 : Short.MIN_VALUE;
        target.i = odd ? 0x76543210 : Integer.MIN_VALUE;
        target.j = odd ? 0x8123456789abcdefL : Long.MIN_VALUE;
        target.f = Float.intBitsToFloat(odd ? 0x7fc01234 : 0x80000000);
        target.d = Double.longBitsToDouble(odd ? 0x7ff8000000001234L : 0x8000000000000000L);
    }
    static long bits(Subject target) {
        switch (shape) {
        case 0: return target.z ? 1 : 0;
        case 1: return target.b;
        case 2: return target.c;
        case 3: return target.s;
        case 4: return target.i ^ (value instanceof String ? 0x13579bdf : 0);
        case 5: return target.j;
        case 6: return Float.floatToRawIntBits(target.f);
        default: return Double.doubleToRawLongBits(target.d);
        }
    }
    static long bits(Object result) {
        switch (shape) {
        case 0: return ((Boolean)result).booleanValue() ? 1 : 0;
        case 1: return ((Byte)result).byteValue();
        case 2: return ((Character)result).charValue();
        case 3: return ((Short)result).shortValue();
        case 4: return ((Integer)result).intValue();
        case 5: return ((Long)result).longValue();
        case 6: return Float.floatToRawIntBits(((Float)result).floatValue());
        default: return Double.doubleToRawLongBits(((Double)result).doubleValue());
        }
    }
    static final class Observer extends Trace {
        public void observe(int phase, Subject target, Object actual) {
            events += phase + ",";
            check(target == receiver && actual == value);
            check(target.state == (phase == 1 ? 7 : 8));
            check(target.saved == (phase == 1 ? initial : value));
            if (phase == 1 && mode == 9) fields(target, attempt + 1);
            if (phase == 2) {
                wanted = bits(target);
                if (mode == 10) fields(target, attempt + 1);
                if (mode == 11) target.saved = alternative;
            }
            if ((phase == 1 && (mode == 4 || mode == 6)) || (phase == 2 && (mode == 5 || mode == 7)))
                PrivateDirectPrimitiveHarness.<RuntimeException>raise(failure);
        }
    }
    static Object call(int route) throws Throwable {
        if (route == 0) return Core.call(receiver, value, shape);
        if (route == 1 || route == 3) {
            try {
                return route == 1 ? Core.class.getMethod("call", Subject.class, Object.class, int.class)
                    .invoke(null, receiver, value, shape)
                    : Subject.class.getMethod("run" + names[shape], Object.class).invoke(receiver, value);
            } catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return MethodHandles.publicLookup().findStatic(Core.class, "call",
            MethodType.methodType(Object.class, Subject.class, Object.class, int.class))
            .invokeWithArguments(receiver, value, shape);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]); shape = Integer.parseInt(args[1]); mode = Integer.parseInt(args[2]);
        Method method = Subject.class.getDeclaredMethod("select" + names[shape], Object.class);
        TypeVariable<?>[] formals = method.getTypeParameters();
        check(Modifier.isPrivate(method.getModifiers()) && !Modifier.isFinal(method.getModifiers()) && !Modifier.isStatic(method.getModifiers()));
        check(formals.length == 1 && formals[0].getBounds().length == 1 && formals[0].getBounds()[0] == Object.class);
        check(method.getGenericParameterTypes()[0].equals(formals[0]) && method.getReturnType() == types[shape]);
        for (attempt = 0; attempt < 2; attempt++) {
            Subject original = new Subject(); original.state = 7; initial = original.saved = new Object(); alternative = new Object(); fields(original, attempt);
            receiver = mode == 8 ? null : original;
            value = mode == 1 ? null : mode == 2 ? new Object() : mode == 3 ? Integer.valueOf(17) : new String("owned seed");
            events = ""; Trace.hook = new Observer();
            failure = mode == 4 || mode == 5 ? new Checked() : new LinkageError("owned error");
            Object result = null; Throwable caught = null;
            try { result = call(route); } catch (Throwable problem) { caught = problem; }
            boolean entered = mode != 8, stored = entered && mode != 4 && mode != 6;
            check(events.equals(entered ? stored ? "1,2," : "1," : ""));
            check(original.state == (stored ? 8 : 7));
            check(original.saved == (stored ? mode == 11 ? alternative : value : initial));
            if (mode == 4 || mode == 5 || mode == 6 || mode == 7) check(caught == failure);
            else if (mode == 8) check(caught instanceof NullPointerException);
            else check(caught == null && bits(result) == wanted);
        }
        System.out.println("attempts=2,private-direct-primitive=matched");
    }
}
