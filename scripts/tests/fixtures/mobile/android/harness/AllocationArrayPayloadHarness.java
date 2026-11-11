import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public class AllocationArrayPayloadHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned allocation payload contract");
    }
    static Target call(int entry, int route, Object input) throws Throwable {
        if (route == 0) {
            if (entry == 0) return Core.create(input);
            if (entry == 1) return Core.guarded(input);
            return Core.locked(input);
        }
        if (route == 1) return AllocationArrayPayloadCaller.call(entry, input);
        String name = entry == 0 ? "create" : entry == 1 ? "guarded" : "locked";
        if (route == 2) {
            try { return (Target) Core.class.getMethod(name, Object.class).invoke(null, input); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (Target) MethodHandles.publicLookup().findStatic(Core.class, name,
                MethodType.methodType(Target.class, Object.class)).invoke(input);
    }
    static void check(Target value) {
        require(java.util.Arrays.equals(value.booleanValues, new boolean[]{false, true, false, true, true, false, true}));
        require(java.util.Arrays.equals(value.byteValues, new byte[]{-128, 127, -1, 0, 17, -23, 63}));
        require(java.util.Arrays.equals(value.charValues, new char[]{'\u0000', '\uffff', 'x', '\u1234', 'y', 'z', 'a'}));
        require(java.util.Arrays.equals(value.shortValues, new short[]{-32768, 32767, -1, 0, 17, -23, 63}));
        require(java.util.Arrays.equals(value.intValues, new int[]{Integer.MIN_VALUE, Integer.MAX_VALUE, -1, 0, 17, -23, 63}));
        require(java.util.Arrays.equals(value.longValues, new long[]{Long.MIN_VALUE, Long.MAX_VALUE, -1L, 0L, 17L, -23L, 63L}));
        require(java.util.Arrays.equals(value.floatValues, new float[]{-0.0f, Float.NaN, Float.POSITIVE_INFINITY, Float.NEGATIVE_INFINITY, Float.MIN_VALUE, Float.MAX_VALUE, -3.25f}));
        require(java.util.Arrays.equals(value.doubleValues, new double[]{-0.0, Double.NaN, Double.POSITIVE_INFINITY, Double.NEGATIVE_INFINITY, Double.MIN_VALUE, Double.MAX_VALUE, -3.25}));
        require(Float.floatToRawIntBits(value.floatValues[0]) == 0x80000000);
        require(Double.doubleToRawLongBits(value.doubleValues[0]) == 0x8000000000000000L);
    }
    public static void main(String[] args) throws Throwable {
        int entry = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        Trace.lock = new Object(); Trace.expectLock = entry == 2;
        Trace.failure = new IllegalArgumentException("owned payload failure");
        Trace.failAt = mode == 1 ? 1 : mode == 2 ? 8 : mode == 3 ? 9 : mode == 5 ? 2 : 0;
        Trace.seed = 19;
        int[] input = {17};
        Object argument = mode == 4 ? null : input;
        Object previous = null;
        for (int attempt = 0; attempt < 2; attempt++) {
            int old = input[0];
            Trace.events = 0; Trace.last = null; Trace.published = null; Trace.seen = null;
            Target result = null; Throwable thrown = null;
            try { result = call(entry, route, argument); } catch (Throwable failure) { thrown = failure; }
            require(!Thread.holdsLock(Trace.lock));
            long expected = attempt == 0 ? 9 : 0;
            if (mode != 3) expected = expected * 10 + 1;
            if (mode != 1 && mode != 3 && mode != 4) expected = expected * 10 + 2;
            boolean constructor = mode == 0 || mode == 2;
            if (constructor) expected = expected * 10 + 8;
            int delta = constructor ? 2 : mode == 5 ? 1 : 0;
            require(Trace.events == expected && input[0] == old + delta);
            require(Trace.seen == (mode != 1 && mode != 3 && mode != 4 ? argument : null));
            Throwable observed = entry == 1 ? Trace.last : thrown;
            require(entry != 1 || thrown == null);
            if (mode == 0) require(observed == null && result == Trace.published);
            else {
                require(entry != 1 || result == null);
                if (mode == 3) {
                    if (attempt == 0) require(observed instanceof ExceptionInInitializerError && observed.getCause() == Trace.failure);
                    else require(observed instanceof NoClassDefFoundError);
                } else if (mode == 4) require(observed instanceof NullPointerException);
                else require(observed == Trace.failure);
            }
            require(constructor ? Trace.published != null : Trace.published == null);
            if (constructor) {
                Target value = Trace.published;
                require(value.before == 41 + old && value.after == old + 1);
                check(value);
                require(value.intValues != previous); previous = value.intValues;
            }
            System.out.println("entry=" + entry + ",route=" + route + ",mode=" + mode
                    + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
