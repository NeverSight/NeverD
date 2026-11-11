import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.Constructor;
import java.lang.reflect.InvocationTargetException;
import kotlin.jvm.internal.DefaultConstructorMarker;

public final class ConstructorParameterReadHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("constructor parameter read changed");
    }
    static final class Alternate extends Provider {
        @Override public int read() {
            Trace.receiver = this;
            Trace.calls++;
            Trace.hit(6);
            ((int[]) Trace.input)[0] += 7;
            Trace.hit(7);
            return Trace.result + 1;
        }
    }
    static Core make(int route, boolean defaults, int mask, int value) throws Throwable {
        if (route == 0) return defaults ? new Core() : new Core(value);
        if (route == 1) return ConstructorParameterReadCaller.make(defaults, value);
        Class<?>[] types = mask >= 0
            ? new Class<?>[]{int.class, int.class, DefaultConstructorMarker.class}
            : defaults ? new Class<?>[]{} : new Class<?>[]{int.class};
        Object[] values = mask >= 0 ? new Object[]{value, mask, null}
            : defaults ? new Object[]{} : new Object[]{value};
        if (route == 2) {
            try { return Core.class.getConstructor(types).newInstance(values); }
            catch (InvocationTargetException error) { throw error.getCause(); }
        }
        return (Core) MethodHandles.lookup().findConstructor(Core.class,
            MethodType.methodType(void.class, types)).invokeWithArguments(values);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), level = Integer.parseInt(args[1]);
        int mask = Integer.parseInt(args[2]), mode = Integer.parseInt(args[3]);
        boolean synthetic = Boolean.parseBoolean(args[4]);
        Constructor<Core> generated = Core.class.getConstructor(int.class, int.class, DefaultConstructorMarker.class);
        require(generated.isSynthetic() == synthetic && generated.getModifiers() == (synthetic ? 0x1001 : 1));
        require(generated.getGenericParameterTypes().length == 3 && generated.getExceptionTypes().length == 0);
        require(Core.class.getDeclaredField("value").getModifiers() == 0x11);
        boolean defaults = mask >= 0 ? (mask & 1) != 0 : level == 1;
        boolean alternate = mode >= 10;
        Provider provider = mode == 1 ? null : alternate ? new Alternate() : new Provider();
        Trace.initialProvider = provider;
        RuntimeException failure = new IllegalArgumentException("owned constructor read failure");
        Trace.failure = failure;
        Trace.failAt = mode == 5 ? 1 : mode == 6 ? 2 : mode == 7 ? 3
            : mode == 8 ? 4 : mode == 9 ? 5 : mode == 11 ? 6 : mode == 12 ? 7 : 0;
        for (int attempt = 0; attempt < 2; attempt++) {
            int supplied = attempt == 0 ? Integer.MIN_VALUE : Integer.MAX_VALUE;
            Trace.result = attempt == 0 ? 73 : Integer.MAX_VALUE;
            int expectedValue = defaults ? Trace.result + (alternate ? 1 : 0) : supplied;
            int[] input = mode == 4 ? new int[0] : new int[]{3};
            Trace.input = mode == 2 ? new Object() : mode == 3 ? null : input;
            Trace.events = 0; Trace.calls = 0; Trace.receiver = null;
            Trace.base = null; Trace.core = null; Trace.baseValue = 0;
            long events = 0;
            int calls = 0, mutation = 0, failedEvent = 0;
            String failedKind = null;
            boolean base = false, core = false;
            if (defaults) {
                if (mode == 5) {
                    events = attempt == 0 ? 1 : 0;
                    failedKind = attempt == 0 ? "init" : "failed-class";
                } else {
                    if (attempt == 0) events = 1;
                    if (provider == null) failedKind = "null";
                    else {
                        int pre = alternate ? 6 : 2, post = alternate ? 7 : 3;
                        calls = 1;
                        events = events * 10 + pre;
                        if (Trace.failAt == pre) failedEvent = pre;
                        else if (mode == 2) failedKind = "cast";
                        else if (mode == 3) failedKind = "null";
                        else if (mode == 4) failedKind = "bounds";
                        else {
                            mutation = alternate ? 7 : 1;
                            events = events * 10 + post;
                            if (Trace.failAt == post) failedEvent = post;
                        }
                    }
                }
            }
            if (failedKind == null && failedEvent == 0) {
                base = true;
                events = events * 10 + 4;
                if (Trace.failAt == 4) failedEvent = 4;
                else {
                    core = true;
                    events = events * 10 + 5;
                    if (Trace.failAt == 5) failedEvent = 5;
                }
            }
            Throwable caught = null;
            Core result = null;
            try { result = make(route, level == 1, mask, supplied); }
            catch (Throwable error) { caught = error; }
            if (failedEvent != 0) require(caught == failure);
            else if ("init".equals(failedKind)) require(caught instanceof ExceptionInInitializerError
                && ((ExceptionInInitializerError) caught).getException() == failure);
            else if ("failed-class".equals(failedKind)) require(caught instanceof NoClassDefFoundError);
            else if ("cast".equals(failedKind)) require(caught != null && caught.getClass() == ClassCastException.class);
            else if ("null".equals(failedKind)) require(caught != null && caught.getClass() == NullPointerException.class);
            else if ("bounds".equals(failedKind)) require(caught != null && caught.getClass() == ArrayIndexOutOfBoundsException.class);
            else require(caught == null && result != null && result == Trace.base && result == Trace.core
                && result.value == expectedValue);
            require(Trace.events == events && Trace.calls == calls && Trace.receiver == (calls == 0 ? null : provider));
            require((Trace.base != null) == base && (Trace.core != null) == core);
            if (base) require(Trace.baseValue == expectedValue && ((Core) Trace.base).value == (core ? expectedValue : 0));
            if (input.length != 0) require(input[0] == 3 + mutation);
            System.out.println("route=" + route + ",level=" + level + ",mask=" + mask + ",mode=" + mode
                + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
