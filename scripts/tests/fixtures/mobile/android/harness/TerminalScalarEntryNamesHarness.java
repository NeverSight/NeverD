import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class TerminalScalarEntryNamesHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned terminal scalar entry names");
    }
    static Core make(int route) throws Throwable {
        if (route == 0) return Client.make();
        if (route == 1) return TerminalScalarEntryNamesCaller.make();
        if (route == 2) {
            try { return (Core) Client.class.getMethod("make").invoke(null); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (Core) MethodHandles.publicLookup().findStatic(Client.class, "make", MethodType.methodType(Core.class)).invokeExact();
    }
    static Object call(int route, int index, Core c, Object value) throws Throwable {
        if (route == 0) {
            if (index == 0) return Client.bytes(c, (Byte) value);
            if (index == 1) return Client.longs(c, (Long) value);
            if (index == 2) return Client.doubles(c, (Double) value);
            if (index == 3) return Client.reject(c, (Integer) value);
            return Client.consume(c, value);
        }
        if (route == 1) {
            if (index == 0) return TerminalScalarEntryNamesCaller.bytes(c, (Byte) value);
            if (index == 1) return TerminalScalarEntryNamesCaller.longs(c, (Long) value);
            if (index == 2) return TerminalScalarEntryNamesCaller.doubles(c, (Double) value);
            if (index == 3) return TerminalScalarEntryNamesCaller.reject(c, (Integer) value);
            return TerminalScalarEntryNamesCaller.consume(c, value);
        }
        String[] names = {"bytes", "longs", "doubles", "reject", "consume"};
        Class<?>[] parameters = {byte.class, long.class, double.class, int.class, Object.class};
        Class<?>[] results = {boolean.class, long.class, double.class, boolean.class, boolean.class};
        Class<?> owner = index == 4 ? Api.class : Core.class;
        if (route == 2) {
            try { return Client.class.getMethod(names[index], owner, parameters[index]).invoke(null, c, value); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return MethodHandles.publicLookup().findStatic(Client.class, names[index],
            MethodType.methodType(results[index], owner, parameters[index])).invoke(c, value);
    }
    static Object named(Core c, String name, int index, Object value, boolean handle) throws Throwable {
        Class<?>[] parameters = {byte.class, long.class, double.class, int.class};
        Class<?>[] results = {boolean.class, long.class, double.class, boolean.class};
        if (handle) return MethodHandles.publicLookup().findVirtual(Core.class, name,
            MethodType.methodType(results[index], parameters[index])).invoke(c, value);
        try { return Core.class.getDeclaredMethod(name, parameters[index]).invoke(c, value); }
        catch (InvocationTargetException failure) { throw failure.getCause(); }
    }
    static void result(int index, Object actual, byte b, long j, long bits) {
        if (index == 0) require(((Boolean) actual).booleanValue() == (b < 0));
        if (index == 1) require(((Long) actual).longValue() == (((j << 17) | (j >>> 47)) ^ 0x123456789abcdef0L));
        if (index == 2) require(Double.doubleToRawLongBits((Double) actual) == bits);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        long[] wide = {0x123456789abcdef0L, Long.MIN_VALUE, Long.MAX_VALUE, -1L, 0x100000000L, 0xffffffff00000000L,
            1L, 2L, 3L, 4L, 5L, 6L, 7L, 8L, 0x100000001L, -0x100000001L};
        long[] bits = {0x3ff8000000000001L, 0x8000000000000000L, 0x7ff8123456789abcL, 0x7ff0000000000000L,
            0xfff0000000000000L, 1L, 0L, 1L, 2L, 3L, 4L, 5L, 6L, 7L, 0xfff923456789abcdL, 0x8000000000000001L};
        for (int attempt = 0; attempt < 2; attempt++) {
            byte b = (byte) (mode * 47 - 128 + attempt);
            long j = wide[mode] + attempt;
            double d = Double.longBitsToDouble(bits[mode]);
            Object[] values = {b, j, d, 0x12345678 + attempt};
            Trace.events = 0;
            Trace.failAt = mode == 5 || mode == 6 ? 1 : mode == 7 || mode == 9 ? 2 : mode == 8 || mode == 10 ? 3 : mode == 11 || mode == 12 ? 4 : 0;
            Trace.fatal = mode == 6 || mode == 9 || mode == 10 || mode == 12;
            Trace.failure = new RuntimeException("owned terminal scalar failure");
            Trace.error = new AssertionError("owned terminal scalar error");
            Core c = null;
            Throwable failure = null;
            try { c = make(route); } catch (Throwable value) { failure = value; }
            if (mode == 5 || mode == 6) {
                require(c == null && failure == (Trace.fatal ? Trace.error : Trace.failure) && Trace.events == 1);
                continue;
            }
            require(failure == null && c != null && Trace.events == 1);
            for (int index = 0; index < 3; index++) {
                Trace.events = 0;
                failure = null;
                Object actual = null;
                try { actual = call(route, index, mode == 13 ? null : c, values[index]); } catch (Throwable value) { failure = value; }
                if (mode == 13) require(failure instanceof NullPointerException && Trace.events == 0);
                else if (Trace.failAt == index + 2) require(failure == (Trace.fatal ? Trace.error : Trace.failure) && Trace.events == index + 2);
                else {
                    require(failure == null && Trace.events == index + 2);
                    result(index, actual, b, j, bits[mode]);
                    for (boolean handle : new boolean[]{false, true}) {
                        Trace.events = 0;
                        result(index, named(c, args[index + 2], index, values[index], handle), b, j, bits[mode]);
                        require(Trace.events == index + 2);
                    }
                }
            }
            Trace.failAt = 0;
            for (int index = 3; index < 5; index++) {
                Throwable previous = null;
                for (int repeat = 0; repeat < 2; repeat++) {
                    Trace.events = 0;
                    failure = null;
                    Object value = index == 3 ? values[3] : mode == 1 ? null : mode == 15 ? c : new Object();
                    try { call(route, index, mode == 14 ? null : c, value); } catch (Throwable thrown) { failure = thrown; }
                    if (mode == 14) require(failure instanceof NullPointerException && Trace.events == 0);
                    else require(failure instanceof UnsupportedOperationException && failure != previous && Trace.events == 0
                        && (index == 3 ? "owned scalar consumer" : "owned erased consumer").equals(failure.getMessage()));
                    previous = failure;
                }
            }
            for (boolean handle : new boolean[]{false, true}) {
                failure = null;
                try { named(c, args[5], 3, values[3], handle); } catch (Throwable value) { failure = value; }
                require(failure instanceof UnsupportedOperationException && "owned scalar consumer".equals(failure.getMessage()));
            }
        }
        System.out.println("attempts=2,terminal-scalar-entries=matched");
    }
}
