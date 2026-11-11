import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class InlinedAccessorNamesHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned inlined accessor names");
    }
    static Core make(int route, byte b, long j, double d) throws Throwable {
        if (route == 0) return Client.make(b, j, d);
        if (route == 1) return InlinedAccessorNamesCaller.make(b, j, d);
        if (route == 2) {
            try { return (Core) Client.class.getMethod("make", byte.class, long.class, double.class).invoke(null, b, j, d); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (Core) MethodHandles.publicLookup().findStatic(Client.class, "make",
            MethodType.methodType(Core.class, byte.class, long.class, double.class)).invokeExact(b, j, d);
    }
    static int compare(int route, Core receiver, Object other) throws Throwable {
        if (route == 0) return ((Api<Object>) receiver).compare(other);
        if (route == 1) return InlinedAccessorNamesCaller.compare(receiver, other);
        if (route == 2) {
            try { return (Integer) Api.class.getMethod("compare", Object.class).invoke(receiver, other); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (int) MethodHandles.publicLookup().findVirtual(Api.class, "compare",
            MethodType.methodType(int.class, Object.class)).invokeExact((Api) receiver, other);
    }
    static Object read(Core receiver, String name, Class<?> type) throws Throwable {
        Method found = null;
        for (Method method : Core.class.getDeclaredMethods()) {
            if (method.getName().equals(name) && method.getReturnType() == type && method.getParameterCount() == 0) {
                require(found == null);
                found = method;
            }
        }
        require(found != null);
        try { return found.invoke(receiver); }
        catch (InvocationTargetException failure) { throw failure.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        long[] wide = {0x123456789abcdef0L, Long.MIN_VALUE, Long.MAX_VALUE, -1L, 0x100000000L, 0xffffffff00000000L,
            1L, 2L, 3L, 4L, 5L, 6L, 7L, 8L, 9L, 0L, 0x100000001L, -0x100000001L};
        long[] bits = {0x3ff8000000000001L, 0x8000000000000000L, 0x7ff8123456789abcL, 0x7ff0000000000000L,
            0xfff0000000000000L, 1L, 0L, 1L, 2L, 3L, 4L, 5L, 6L, 7L, 8L, 0xfff923456789abcdL,
            0x8000000000000001L, 0x8000000000000000L};
        for (int attempt = 0; attempt < 2; attempt++) {
            byte b = (byte) (mode * 47 - 128), otherB = attempt == 0 ? b : (byte) 1;
            long j = wide[mode], otherJ = attempt == 0 ? j : -1L;
            double d = Double.longBitsToDouble(bits[mode]), otherD = attempt == 0 ? d : 2.0;
            Core peer = new Core(otherB, otherJ, otherD);
            Trace.events = 0;
            Trace.failAt = mode == 9 ? 1 : mode == 10 || mode == 13 ? 2 : mode == 11 || mode == 14 ? 3 : 0;
            Trace.fatal = mode == 13 || mode == 14;
            Trace.failure = new RuntimeException("owned inlined failure");
            Trace.error = new AssertionError("owned inlined error");
            Core receiver = null;
            Throwable failure = null;
            try { receiver = make(route, b, j, d); } catch (Throwable value) { failure = value; }
            if (mode == 9) {
                require(receiver == null && failure == Trace.failure && Trace.events == 1);
                continue;
            }
            require(failure == null && receiver != null && Trace.events == 1);
            require(InlinedAccessorNamesCaller.byteValue(receiver) == b);
            require(InlinedAccessorNamesCaller.longValue(receiver) == j);
            require(Double.doubleToRawLongBits(InlinedAccessorNamesCaller.doubleValue(receiver)) == bits[mode]);
            require(((Byte) read(receiver, args[2], byte.class)).byteValue() == b);
            require(((Long) read(receiver, args[3], long.class)).longValue() == j);
            require(Double.doubleToRawLongBits(((Double) read(receiver, args[4], double.class)).doubleValue()) == bits[mode]);
            require(Trace.events == 1);
            Object other = mode == 6 ? null : mode == 7 ? new Object() : mode == 12 ? receiver : peer;
            if (mode == 8) receiver = null;
            int result = 0;
            failure = null;
            try { result = compare(route, receiver, other); } catch (Throwable value) { failure = value; }
            if (mode == 6 || mode == 8) require(failure instanceof NullPointerException && Trace.events == 1);
            else if (mode == 7) require(failure instanceof ClassCastException && Trace.events == 1);
            else if (mode == 10 || mode == 11 || mode == 13 || mode == 14) {
                require(failure == (Trace.fatal ? Trace.error : Trace.failure));
                require(Trace.events == (Trace.failAt == 2 ? 12 : 123));
            } else {
                int expected = mode == 12 ? 0 : Integer.compare(b & 255, otherB & 255) * 100
                    + Long.compare(j, otherJ) * 10 + Double.compare(d, otherD);
                require(failure == null && result == expected && Trace.events == 123);
            }
        }
        System.out.println("attempts=2,inlined-accessors=matched");
    }
}
