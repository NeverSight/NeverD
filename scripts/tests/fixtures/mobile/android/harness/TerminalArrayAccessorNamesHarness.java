import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
import java.util.Arrays;
public final class TerminalArrayAccessorNamesHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned terminal array accessor names");
    }
    static Core make(int route, byte[] b, short[] s, int[] i, long[] j) throws Throwable {
        if (route == 0) return Client.make(b, s, i, j);
        if (route == 1) return TerminalArrayAccessorNamesCaller.make(b, s, i, j);
        if (route == 2) {
            try { return (Core) Client.class.getMethod("make", byte[].class, short[].class, int[].class, long[].class).invoke(null, b, s, i, j); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (Core) MethodHandles.publicLookup().findStatic(Client.class, "make",
            MethodType.methodType(Core.class, byte[].class, short[].class, int[].class, long[].class)).invokeExact(b, s, i, j);
    }
    static Object read(int route, int index, Core c) throws Throwable {
        if (route == 0) {
            if (index == 0) return Client.bytes(c);
            if (index == 1) return Client.shorts(c);
            if (index == 2) return Client.ints(c);
            return Client.longs(c);
        }
        if (route == 1) {
            if (index == 0) return TerminalArrayAccessorNamesCaller.bytes(c);
            if (index == 1) return TerminalArrayAccessorNamesCaller.shorts(c);
            if (index == 2) return TerminalArrayAccessorNamesCaller.ints(c);
            return TerminalArrayAccessorNamesCaller.longs(c);
        }
        String[] names = {"bytes", "shorts", "ints", "longs"};
        Class<?>[] types = {byte[].class, short[].class, int[].class, long[].class};
        if (route == 2) {
            try { return Client.class.getMethod(names[index], Core.class).invoke(null, c); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return MethodHandles.publicLookup().findStatic(Client.class, names[index],
            MethodType.methodType(types[index], Core.class)).invoke(c);
    }
    static Object named(Core c, String name, Class<?> type, boolean handle) throws Throwable {
        if (handle) return MethodHandles.publicLookup().findVirtual(Core.class, name,
            MethodType.methodType(type)).invoke(c);
        Method found = null;
        for (Method method : Core.class.getDeclaredMethods()) {
            if (method.getName().equals(name) && method.getReturnType() == type && method.getParameterCount() == 0) {
                require(found == null);
                found = method;
            }
        }
        require(found != null);
        try { return found.invoke(c); }
        catch (InvocationTargetException failure) { throw failure.getCause(); }
    }
    static boolean consume(int route, Core c, Object value) throws Throwable {
        if (route == 0) return Client.consume(c, value);
        if (route == 1) return TerminalArrayAccessorNamesCaller.consume(c, value);
        if (route == 2) {
            try { return (Boolean) Client.class.getMethod("consume", Api.class, Object.class).invoke(null, c, value); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (boolean) MethodHandles.publicLookup().findStatic(Client.class, "consume",
            MethodType.methodType(boolean.class, Api.class, Object.class)).invokeExact((Api) c, value);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        for (int attempt = 0; attempt < 2; attempt++) {
            byte[] b = mode == 1 ? null : mode == 2 ? new byte[0] : new byte[]{(byte) -128, (byte) (mode + attempt), 127};
            short[] s = mode == 1 ? null : mode == 2 ? new short[0] : new short[]{Short.MIN_VALUE, (short) (0x1234 + attempt), Short.MAX_VALUE};
            int[] i = mode == 1 ? null : mode == 2 ? new int[0] : new int[]{Integer.MIN_VALUE, 0x12345678 + attempt, Integer.MAX_VALUE};
            long[] j = mode == 1 ? null : mode == 2 ? new long[0] : new long[]{Long.MIN_VALUE, 0x123456789abcdef0L + attempt, Long.MAX_VALUE};
            byte[] beforeB = b == null ? null : b.clone();
            short[] beforeS = s == null ? null : s.clone();
            int[] beforeI = i == null ? null : i.clone();
            long[] beforeJ = j == null ? null : j.clone();
            Object[] arrays = {b, s, i, j};
            Class<?>[] types = {byte[].class, short[].class, int[].class, long[].class};
            Trace.events = 0;
            Trace.failAt = mode == 5 || mode == 6 ? 1 : mode == 7 || mode == 9 ? 2 : mode == 8 || mode == 10 ? 3 : 0;
            Trace.fatal = mode == 6 || mode == 9 || mode == 10;
            Trace.failure = new RuntimeException("owned terminal array failure");
            Trace.error = new AssertionError("owned terminal array error");
            Core c = null;
            Throwable failure = null;
            try { c = make(route, b, s, i, j); } catch (Throwable value) { failure = value; }
            if (mode == 5 || mode == 6) {
                require(c == null && failure == (Trace.fatal ? Trace.error : Trace.failure) && Trace.events == 1);
                continue;
            }
            require(failure == null && c != null && Trace.events == 1);
            for (int index = 0; index < 4; index++) {
                Trace.events = 0;
                failure = null;
                Object actual = null;
                try { actual = read(route, index, mode == 11 ? null : c); } catch (Throwable value) { failure = value; }
                if (mode == 11) require(failure instanceof NullPointerException && Trace.events == 2);
                else if (mode >= 7 && mode <= 10) {
                    require(failure == (Trace.fatal ? Trace.error : Trace.failure));
                    require(Trace.events == (Trace.failAt == 2 ? 2 : 23));
                } else {
                    require(failure == null && actual == arrays[index] && Trace.events == 23);
                    require(named(c, args[index + 2], types[index], false) == arrays[index]);
                    require(named(c, args[index + 2], types[index], true) == arrays[index]);
                    require(Trace.events == 23);
                }
            }
            require(Arrays.equals(b, beforeB) && Arrays.equals(s, beforeS) && Arrays.equals(i, beforeI) && Arrays.equals(j, beforeJ));
            Trace.failAt = 0;
            if (mode == 3) {
                b[0] = 42; s[0] = 12345; i[0] = 0x13579bdf; j[0] = 0x23456789abcdef01L;
                for (int index = 0; index < 4; index++) require(read(route, index, c) == arrays[index]);
            }
            Throwable previous = null;
            for (int call = 0; call < 2; call++) {
                Trace.events = 0;
                failure = null;
                Object value = mode == 12 ? null : mode == 14 ? c : new Object();
                try { consume(route, mode == 15 ? null : c, value); } catch (Throwable thrown) { failure = thrown; }
                if (mode == 15) require(failure instanceof NullPointerException && Trace.events == 4);
                else require(failure instanceof UnsupportedOperationException && failure != previous
                    && "owned array consumer".equals(failure.getMessage()) && Trace.events == 4);
                previous = failure;
            }
        }
        System.out.println("attempts=2,terminal-array-accessors=matched");
    }
}
