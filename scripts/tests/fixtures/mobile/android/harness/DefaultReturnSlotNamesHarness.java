import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public class DefaultReturnSlotNamesHarness {
    static int calls, failures;
    public static class Derived extends Reader {
        public Derived() { super(new State(null)); }
        @Override public void observe(Object[] values, long wide, double real) {
            DefaultTrace.overrides++;
            super.observe(values, wide, real);
        }
    }
    static void check(boolean value) {
        if (!value) throw new AssertionError("default return-slot contract");
    }
    static void call(int route, Reader receiver, Object[] values, long wide, double real) throws Throwable {
        if (route == 0) { receiver.observe(values, wide, real); return; }
        if (route == 1) { ((DefaultApi)receiver).observe(values, wide, real); return; }
        if (route == 2 || route == 3) {
            Method entry = (route == 2 ? DefaultApi.class : Reader.class)
                .getMethod("observe", Object[].class, long.class, double.class);
            check(entry.getDeclaringClass() == DefaultApi.class && entry.isDefault() && !entry.isBridge());
            try { entry.invoke(receiver, values, wide, real); }
            catch (InvocationTargetException error) { throw error.getCause(); }
            return;
        }
        MethodHandle entry = MethodHandles.lookup().findVirtual(Reader.class, "observe",
            MethodType.methodType(void.class, Object[].class, long.class, double.class));
        entry.invokeExact(receiver, values, wide, real);
    }
    public static void main(String[] args) throws Throwable {
        PlainSdkReturnSlotsHarness.main(args);
        for (Class<?> owner : new Class<?>[]{Reader.class, ForwardReader.class, Overriding.class})
            for (Method method : owner.getDeclaredMethods()) check(!method.getName().equals("observe"));
        for (int route = 0; route < 5; route++)
            for (boolean derived : new boolean[]{false,true})
                for (int mode = 0; mode < 6; mode++) {
                    Reader receiver = derived ? new Derived() : new Reader(new State(null));
                    Object[] shared = new Object[]{new Object()};
                    for (int attempt = 0; attempt < 2; attempt++) {
                        Object value = mode == 1 ? null : new Object();
                        shared[0] = value;
                        Object[] values = mode == 3 ? null : mode == 4 ? new Object[0] : shared;
                        long wide = attempt == 0 ? 0x12345678fedcba98L : 0xfedcba9876543210L;
                        double real = attempt == 0 ? Double.longBitsToDouble(0x7ff8000000001234L) : -0.0d;
                        Object before = new Object();
                        DefaultTrace.calls = 0; DefaultTrace.events = 0; DefaultTrace.overrides = 0;
                        DefaultTrace.seen = before;
                        RuntimeException failure = new IllegalStateException("owned-default-failure");
                        DefaultTrace.failure = mode >= 2 ? failure : null;
                        Throwable caught = null;
                        try { call(route, mode == 5 ? null : receiver, values, wide, real); }
                        catch (Throwable error) { caught = error; }
                        calls++;
                        if (mode >= 2) {
                            failures++;
                            if (mode == 2) check(caught == failure && DefaultTrace.seen == value);
                            else check(caught != null && caught.getClass() == (mode == 4 ? ArrayIndexOutOfBoundsException.class : NullPointerException.class)
                                && DefaultTrace.seen == before);
                        } else check(caught == null && DefaultTrace.seen == value);
                        check(DefaultTrace.calls == (mode == 5 ? 0 : 1));
                        check(DefaultTrace.overrides == (derived && mode != 5 ? 1 : 0));
                        check(DefaultTrace.events == (mode == 5 ? 0 : mode >= 2 ? 1 : 12));
                        if (mode != 5) check(DefaultTrace.values == values && DefaultTrace.wide == wide
                            && Double.doubleToRawLongBits(DefaultTrace.real) == Double.doubleToRawLongBits(real));
                    }
                }
        check(calls == 120 && failures == 80);
        System.out.println("default-calls="+calls+",failures="+failures+",original-provider-and-class-dispatch=matched");
    }
}
