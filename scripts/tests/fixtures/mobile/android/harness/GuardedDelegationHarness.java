import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.Constructor;
import java.lang.reflect.InvocationTargetException;
import kotlin.jvm.internal.DefaultConstructorMarker;
public final class GuardedDelegationHarness {
    static void require(boolean value) { if (!value) throw new AssertionError("guarded delegation, final fields and effect order"); }
    static Core make(int level, int route, int mask, Object token, int count, long wide, double number) throws Throwable {
        if (route == 0) {
            if (level == 0) return new Core(token, count, wide, number);
            if (level == 1) return new Core(token, count, wide);
            if (level == 2) return new Core(token, count);
            return new Core(token);
        }
        if (route == 1) return GuardedDelegationCaller.make(level, token, count, wide, number);
        Class<?>[] types = mask < 0 ?
            level == 0 ? new Class<?>[]{Object.class, int.class, long.class, double.class} :
            level == 1 ? new Class<?>[]{Object.class, int.class, long.class} :
            level == 2 ? new Class<?>[]{Object.class, int.class} : new Class<?>[]{Object.class} :
            new Class<?>[]{Object.class, int.class, long.class, double.class, int.class, DefaultConstructorMarker.class};
        Object[] values = mask < 0 ?
            level == 0 ? new Object[]{token, count, wide, number} :
            level == 1 ? new Object[]{token, count, wide} :
            level == 2 ? new Object[]{token, count} : new Object[]{token} :
            new Object[]{token, count, wide, number, mask, null};
        if (route == 2) {
            try { return Core.class.getConstructor(types).newInstance(values); }
            catch (InvocationTargetException error) { throw error.getCause(); }
        }
        return (Core) MethodHandles.lookup().findConstructor(Core.class, MethodType.methodType(void.class, types)).invokeWithArguments(values);
    }
    public static void main(String[] args) throws Throwable {
        int level = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]), mask = Integer.parseInt(args[2]), mode = Integer.parseInt(args[3]);
        boolean synthetic = Boolean.parseBoolean(args[4]);
        Constructor<Core> defaultCtor = Core.class.getConstructor(Object.class, int.class, long.class, double.class, int.class, DefaultConstructorMarker.class);
        require(defaultCtor.isSynthetic() == synthetic);
        int defaults = mask < 0 ? (level == 0 ? 0 : level == 1 ? 8 : level == 2 ? 12 : 14) : mask;
        RuntimeException original = new IllegalArgumentException("owned constructor failure");
        Trace.failure = original; Trace.failAt = mode;
        Core previous = null;
        for (int variant = 0; variant < 3; variant++) {
            Object token = variant == 2 ? null : new Object();
            int count = variant == 0 ? Integer.MIN_VALUE : Integer.MAX_VALUE;
            long wide = variant == 0 ? Long.MAX_VALUE : Long.MIN_VALUE;
            double number = variant == 0 ? -0.0 : Double.longBitsToDouble(0x7ff8000000000023L);
            Trace.events = 0; Trace.observed = null;
            long events = 0;
            int failed = 0;
            boolean earlyNull = token == null && mask < 0;
            if (!earlyNull) {
                for (int event = 1; event <= 3; event++) {
                    if ((defaults & (1 << event)) == 0) continue;
                    events = events * 10 + event;
                    if (mode == event) { failed = event; break; }
                }
                if (failed == 0 && token != null) {
                    events = events * 10 + 4;
                    if (mode == 4) failed = 4;
                    else { events = events * 10 + 5; if (mode == 5) failed = 5; }
                }
            }
            try {
                Core result = make(level, route, mask, token, count, wide, number);
                require(failed == 0 && token != null && result != previous && result == Trace.observed);
                require(result.token == token && result.stamp == 5);
                require(result.count == ((defaults & 2) == 0 ? count : 17));
                require(result.wide == ((defaults & 4) == 0 ? wide : Long.MIN_VALUE + 11));
                require(Double.doubleToRawLongBits(result.number) == ((defaults & 8) == 0 ? Double.doubleToRawLongBits(number) : 0x7ff8000000000042L));
                previous = result;
            } catch (Throwable error) {
                if (failed != 0) require(error == original);
                else require(token == null && error.getClass() == NullPointerException.class);
                require((failed == 5) == (Trace.observed != null));
            }
            require(Trace.events == events);
        }
        System.out.println("level=" + level + ",route=" + route + ",mask=" + mask + ",mode=" + mode + ":matched");
    }
}
