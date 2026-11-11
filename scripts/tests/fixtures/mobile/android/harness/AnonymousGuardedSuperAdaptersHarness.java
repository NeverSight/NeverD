import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;

public final class AnonymousGuardedSuperAdaptersHarness {
    private static int values, identities, states, failures;
    private static void same(Object expected, Object actual) {
        if (expected != actual) throw new AssertionError("identity changed"); identities++;
    }
    private static void state(int expected, int actual) {
        if (expected != actual) throw new AssertionError("state changed " + expected + ":" + actual); states++;
    }
    private static void value(int expected, int actual) {
        if (expected != actual) throw new AssertionError("value changed"); values++;
    }
    private static int call(int route, boolean index, Api<String> receiver, Object argument) throws Throwable {
        try {
            String name = index ? "index" : "contains";
            if (route == 0) return index ? receiver.index(argument) : receiver.contains(argument) ? 1 : 0;
            if (route == 1 || route == 2) {
                Method method = (route == 1 ? receiver.getClass() : Api.class).getMethod(name, Object.class);
                method.setAccessible(true); Object result = method.invoke(receiver, argument);
                return index ? (Integer) result : (Boolean) result ? 1 : 0;
            }
            if (route == 3) {
                MethodHandle handle = MethodHandles.lookup().findVirtual(Api.class, name,
                        MethodType.methodType(index ? int.class : boolean.class, Object.class));
                return index ? (int) handle.invokeExact((Api) receiver, argument)
                             : (boolean) handle.invokeExact((Api) receiver, argument) ? 1 : 0;
            }
            return index ? Caller.index(receiver, argument) : Caller.contains(receiver, argument) ? 1 : 0;
        } catch (InvocationTargetException error) { throw error.getCause(); }
    }
    private static void check(Host host, Api<?> receiver, int[] marker, int calls, int contains, int indices) {
        state(calls, host.calls); state(7 + calls * 3, marker[0]);
        state(contains, receiver.containsCalls); state(indices, receiver.indexCalls);
        same(marker, host.marker); same(calls == 0 ? null : marker, host.observed);
    }
    private static void run(int route, boolean index) throws Throwable {
        Host host = new Host(); int[] marker = {7}; host.marker = marker;
        Api<String> receiver = host.make(); int calls = 0;
        for (Object invalid : new Object[]{null, new Object(), Integer.valueOf(9)}) {
            value(index ? -1 : 0, call(route, index, receiver, invalid));
            check(host, receiver, marker, 0, 0, 0); same(null, host.seen); same(null, receiver.seen);
        }
        String first = new String("first"), second = new String("second");
        for (String input : new String[]{first, second, first}) {
            value(index ? 7 : 1, call(route, index, receiver, input));
            calls++; check(host, receiver, marker, calls, index ? 0 : calls, index ? calls : 0);
            same(input, host.seen); same(input, receiver.seen);
        }
        RuntimeException failure = new RuntimeException("captured callback"); host.failure = failure;
        try { call(route, index, receiver, second); throw new AssertionError("callback failure missing"); }
        catch (RuntimeException error) { same(failure, error); failures++; }
        calls++; check(host, receiver, marker, calls, index ? 0 : calls, index ? calls : 0);
        same(second, host.seen); same(second, receiver.seen); host.failure = null;
        value(index ? 7 : 1, call(route, index, receiver, first));
        calls++; check(host, receiver, marker, calls, index ? 0 : calls, index ? calls : 0);
        Method typed = receiver.getClass().getMethod(index ? "index" : "contains", String.class); typed.setAccessible(true);
        Object result = typed.invoke(receiver, new Object[]{null});
        value(index ? 7 : 1, index ? (Integer) result : (Boolean) result ? 1 : 0);
        calls++; check(host, receiver, marker, calls, index ? 0 : calls, index ? calls : 0);
        same(null, host.seen); same(null, receiver.seen);
        Api<String> sibling = host.make();
        value(index ? 7 : 1, call(route, index, sibling, second));
        calls++; check(host, sibling, marker, calls, index ? 0 : 1, index ? 1 : 0);
        same(second, host.seen); same(second, sibling.seen);
        Host other = new Host(); int[] otherMarker = {7}; other.marker = otherMarker;
        Api<String> separate = other.make();
        value(index ? 7 : 1, call(route, index, separate, first));
        check(other, separate, otherMarker, 1, index ? 0 : 1, index ? 1 : 0);
        check(host, sibling, marker, calls, index ? 0 : 1, index ? 1 : 0);
        try { call(route, index, null, first); throw new AssertionError("null receiver accepted"); }
        catch (NullPointerException expected) { failures++; }
    }
    private static void constructorMapping(boolean original) throws Throwable {
        Host captured = new Host(), unused = new Host();
        int[] marker = {7}; captured.marker = marker; unused.marker = new int[]{7};
        Class<?> local = captured.make().getClass();
        Constructor<?>[] constructors = local.getDeclaredConstructors();
        if (constructors.length != 1 || constructors[0].getParameterCount() != (original ? 1 : 2))
            throw new AssertionError("unreported local constructor shape");
        for (Class<?> parameter : constructors[0].getParameterTypes())
            if (parameter != Host.class) throw new AssertionError("capture type changed");
        constructors[0].setAccessible(true);
        @SuppressWarnings("unchecked") Api<String> receiver = (Api<String>) constructors[0].newInstance(
                original ? new Object[]{captured} : new Object[]{unused, captured});
        String input = new String("captured"); value(1, receiver.contains(input) ? 1 : 0);
        check(captured, receiver, marker, 1, 1, 0); same(input, captured.seen); same(input, receiver.seen);
        state(0, unused.calls); state(7, unused.marker[0]);
        value(-1, receiver.index(null)); check(captured, receiver, marker, 1, 1, 0);
    }
    public static void main(String[] args) throws Throwable {
        for (int route = 0; route < 5; route++) { run(route, false); run(route, true); }
        constructorMapping(args[0].equals("original"));
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("closed-anonymous-scope=retained,captured-host-aliases-and-super-dispatch=original,bridge-and-local-metadata=reported");
    }
}
