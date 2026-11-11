import fixture.Client;
import fixture.Entry;
import fixture.Trace;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;

public final class LocalSetterHarness {
    private static void require(boolean value) {
        if (!value) throw new AssertionError("owned local void setter changed");
    }
    private static Object run(int route, int operation, Entry<Object,Object> entry, Object input, String name) throws Throwable {
        try {
            if (route == 0) {
                if (operation == 0) return Client.exchange(entry, input);
                Client.write(entry, input); return null;
            }
            if (operation == 1) {
                if (route == 1) LocalSetterCaller.write(entry, input, name);
                else if (route == 2) {
                    Method selected = null;
                    for (Method method : Entry.class.getDeclaredMethods())
                        if (method.getName().equals(name) && method.getReturnType() == void.class
                                && java.util.Arrays.equals(method.getParameterTypes(), new Class<?>[]{Object.class})) selected = method;
                    require(selected != null); selected.invoke(entry, input);
                } else MethodHandles.publicLookup().findVirtual(Entry.class, name,
                        MethodType.methodType(void.class, Object.class)).invokeExact(entry, input);
                Trace.hit(9); return null;
            }
            Object output;
            if (route == 1) output = LocalSetterCaller.exchange(entry, input);
            else if (route == 2) output = java.util.Map.Entry.class.getMethod("setValue", Object.class).invoke(entry, input);
            else output = MethodHandles.publicLookup().findVirtual(java.util.Map.Entry.class, "setValue",
                    MethodType.methodType(Object.class, Object.class)).invoke(entry, input);
            Trace.after = output; Trace.hit(7); return output;
        } catch (InvocationTargetException error) { throw error.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        String name = args[0];
        int route = Integer.parseInt(args[1]), operation = Integer.parseInt(args[2]), mode = Integer.parseInt(args[3]);
        for (int attempt = 0; attempt < 2; ++attempt) {
            Object key = mode == 19 ? null : new Object();
            Object previous = mode == 18 ? null : mode == 20 ? Integer.valueOf(513) : new Object();
            Object input = mode == 17 ? null : new Object(), replacement = new Object();
            int fail = mode >= 2 && mode <= 15 ? 1 + (mode - 2) / 2 : 0;
            Throwable failure = mode >= 2 && mode <= 15 && mode % 2 == 1
                    ? new AssertionError("sentinel") : new IllegalStateException("sentinel");
            Trace.events = 0; Trace.fail = fail; Trace.failure = failure;
            Trace.mutate = mode == 1 || fail == 5; Trace.replacement = replacement;
            Trace.seenEntry = null; Trace.seenKey = null; Trace.seenValue = null;
            Trace.beforeCallback = null; Trace.after = null;
            Entry<Object,Object> actual = new Entry<Object,Object>(key, previous);
            Entry<Object,Object> entry = mode == 16 ? null : actual;
            Object output = null; Throwable caught = null;
            try { output = run(route, operation, entry, input, name); }
            catch (Throwable error) { caught = error; }
            int events = attempt == 0 ? 8 : 0, last = 0;
            if (entry != null) {
                int[] steps = operation == 1 ? new int[]{9}
                        : Trace.mutate ? new int[]{1,2,3,4,5,6,7} : new int[]{1,2,3,4,6,7};
                for (int step : steps) { events = events * 10 + step; last = step; if (step == fail) break; }
            }
            require(Trace.events == events);
            if (entry == null) require(caught instanceof NullPointerException);
            else require(caught == (fail == 0 || operation == 1 ? null : failure));
            Object wanted = entry == null || operation == 0 && last < 3 ? previous
                    : operation == 0 && Trace.mutate && last >= 5 ? replacement : input;
            require(actual.peek() == wanted);
            require(Trace.seenEntry == (operation == 0 && last >= 4 ? actual : null));
            if (operation == 0 && last >= 4) {
                require(Trace.seenKey == key && Trace.seenValue == input && Trace.beforeCallback == input);
            }
            require(Trace.after == (operation == 0 && last == 7 ? previous : null));
            require(output == (operation == 0 && caught == null ? previous : null));
            System.out.println("route=" + route + ",operation=" + operation + ",mode=" + mode + ",attempt=" + attempt + ",events=" + events);
        }
    }
}
