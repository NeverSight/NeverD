import fixture.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class DelegationArgumentHarness {
    static void require(boolean ok) { if (!ok) throw new AssertionError("delegation argument order"); }
    static Core make(int route, Input value) throws Throwable {
        try {
            if (route == 0) return new Core(value);
            if (route == 1) return DelegationArgumentCaller.make(value);
            if (route == 2) return Core.class.getConstructor(Input.class).newInstance(value);
            return (Core)MethodHandles.publicLookup().findConstructor(Core.class, MethodType.methodType(void.class, Input.class)).invokeExact(value);
        } catch (InvocationTargetException failure) { throw failure.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        int mode = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]);
        RuntimeException original = new IllegalArgumentException("owned delegation failure");
        Trace.failAt = mode; Trace.failure = original;
        long expected = 0; Core previous = null;
        for (int attempt = 1; attempt <= 2; attempt++) {
            Input value = new Input();
            int a = attempt == 1 ? 0x7fc01234 : 0x80000000;
            int b = attempt == 1 ? 0xffc05678 : 0x7f800000;
            int c = attempt == 1 ? 1 : 0xff800000;
            value.a = Float.intBitsToFloat(a); value.b = Float.intBitsToFloat(b); value.c = Float.intBitsToFloat(c);
            value.edge = attempt == 1 ? Integer.MIN_VALUE : Integer.MAX_VALUE;
            try {
                Core result = make(route, value);
                require(mode == 0 && result != previous && Float.floatToRawIntBits(result.x) == a
                        && Float.floatToRawIntBits(result.y) == b && Float.floatToRawIntBits(result.z) == c
                        && result.edge == value.edge && value.calls == 4 && value.first != value.later
                        && value.later == Provider.INSTANCE);
                previous = result;
            } catch (Throwable failure) {
                require(mode != 0);
                if (mode <= 2) {
                    if (attempt == 1) require(failure.getClass() == ExceptionInInitializerError.class && failure.getCause() == original);
                    else require(failure.getClass() == NoClassDefFoundError.class);
                } else require(failure == original);
                require(value.calls == (mode <= 4 ? 0 : Math.min(mode - 4, 4)));
            }
            if (attempt == 1 || mode > 2 || mode == 0)
                for (int event = attempt == 1 ? 1 : 3; event <= (mode == 0 ? 8 : mode); event++) expected = expected * 10 + event;
            require(Trace.events == expected);
        }
        System.out.println("mode=" + mode + ",route=" + route + ",trace=" + Trace.events);
    }
}
