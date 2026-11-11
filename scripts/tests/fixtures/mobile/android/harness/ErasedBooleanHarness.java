import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class ErasedBooleanHarness {
    static final class Source implements Creator<Object> {
        Object value;
        public Object read() { Trace.reads++; Trace.hit(3); return value; }
    }
    static final class Value implements fixture.Readable {
        public boolean accept(int key, long flags, double fraction) {
            Trace.accepts++; Trace.flags = flags; Trace.bits = Double.doubleToRawLongBits(fraction); Trace.hit(5);
            return key >= 0;
        }
    }
    static void require(boolean condition) {
        if (!condition) throw new AssertionError("original generic boolean result and effects");
    }
    static boolean call(Root receiver, Object value, Source creator, int key, long flags, double fraction, int flavor, int route) throws Throwable {
        if (route == 0) return flavor == 0 ? Client.direct(receiver, key, value) : Client.checked(creator, key, flags, fraction);
        if (route == 1) return flavor == 0 ? ErasedBooleanCaller.direct(receiver, key, value) : ErasedBooleanCaller.checked(creator, key, flags, fraction);
        if (route == 2) {
            try {
                return (Boolean) (flavor == 0 ? Client.class.getMethod("direct", Root.class, int.class, Object.class).invoke(null, receiver, key, value)
                    : Client.class.getMethod("checked", Creator.class, int.class, long.class, double.class).invoke(null, creator, key, flags, fraction));
            } catch (InvocationTargetException error) { throw error.getCause(); }
        }
        return flavor == 0
            ? (boolean) MethodHandles.lookup().findStatic(Client.class, "direct", MethodType.methodType(boolean.class, Root.class, int.class, Object.class)).invokeExact(receiver, key, value)
            : (boolean) MethodHandles.lookup().findStatic(Client.class, "checked", MethodType.methodType(boolean.class, Creator.class, int.class, long.class, double.class)).invokeExact((Creator) creator, key, flags, fraction);
    }
    public static void main(String[] args) throws Throwable {
        int flavor = Integer.parseInt(args[0]), variant = Integer.parseInt(args[1]);
        int route = Integer.parseInt(args[2]), mode = Integer.parseInt(args[3]);
        Object value = variant == 1 ? null : new Object();
        Source creator = new Source();
        creator.value = variant == 1 ? null : variant == 2 ? new Object() : new Value();
        int key = flavor == 0 && variant == 2 || flavor == 1 && variant == 4 ? Integer.MIN_VALUE : Integer.MAX_VALUE;
        RuntimeException original = new IllegalArgumentException("owned boolean failure");
        Trace.failure = original; Trace.failAt = mode;
        long events = 0;
        int calls = 0, reads = 0, accepts = 0;
        boolean nullReceiver = flavor == 0 && variant == 3;
        for (int attempt = 1; attempt <= 2; attempt++) {
            long flags = attempt == 1 ? Long.MIN_VALUE : Long.MAX_VALUE;
            long bits = attempt == 1 ? 0x7ff8000000000015L : 0x8000000000000000L;
            int failed = 0;
            if (!nullReceiver) {
                if (attempt == 1) events = 1;
                if (mode == 1) failed = 1;
                else {
                    events = events * 10 + 2; calls++;
                    if (mode == 2 || flavor == 1 && variant == 3) failed = 2;
                    else if (flavor == 0) {
                        if (key >= 0) { events = events * 10 + 3; if (mode == 3) failed = 3; }
                    } else {
                        events = events * 10 + 3; reads++;
                        if (mode == 3 || variant == 2) failed = 3;
                        else {
                            events = events * 10 + 4;
                            if (mode == 4 || variant == 1) failed = 4;
                            else { events = events * 10 + 5; accepts++; if (mode == 5) failed = 5; }
                        }
                    }
                }
            }
            try {
                Root receiver = flavor == 0 && !nullReceiver ? new Root() : null;
                boolean actual = call(receiver, value, variant == 3 ? null : creator, key, flags, Double.longBitsToDouble(bits), flavor, route);
                boolean expected = flavor == 0 ? key >= 0 && value != null : key >= 0;
                require(!nullReceiver && failed == 0 && actual == expected);
            } catch (Throwable error) {
                if (mode == 1) {
                    if (attempt == 1) require(error.getClass() == ExceptionInInitializerError.class && error.getCause() == original);
                    else require(error.getClass() == NoClassDefFoundError.class);
                } else if (failed != 0 && mode == failed) require(error == original);
                else require((nullReceiver || flavor == 1 && variant >= 1 && variant <= 3)
                    && error.getClass() == (flavor == 1 && variant == 2 ? ClassCastException.class : NullPointerException.class));
            }
            require(Trace.events == events && Trace.calls == calls && Trace.reads == reads && Trace.accepts == accepts);
            if (flavor == 0 && calls > 0) require(Trace.value == value);
            if (accepts > 0) require(Trace.flags == flags && Trace.bits == bits);
        }
        System.out.println("flavor=" + flavor + ",variant=" + variant + ",route=" + route + ",mode=" + mode + ",trace=" + Trace.events);
    }
}
