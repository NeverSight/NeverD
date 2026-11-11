import fixture.Action;
import fixture.ArgAction;
import fixture.ArgWorker;
import fixture.Trace;
import fixture.Value;
import fixture.Worker;
import java.lang.reflect.Method;

public class VoidUnitBridgeHarness {
    static int identities;
    static int failures;
    static void check(boolean condition) {
        if (!condition) throw new AssertionError();
    }
    static void same(Object actual, Object expected) {
        check(actual == expected);
        ++identities;
    }
    static void reflected(Class<?> type, String name, Class<?> parameter) {
        int voids = 0;
        int bridges = 0;
        int typed = 0;
        for (Method method : type.getDeclaredMethods()) {
            if (method.getName().equals(name) && method.getReturnType() == void.class
                    && method.getParameterCount() == (parameter == null ? 0 : 1)) ++voids;
            if (method.isBridge()) {
                check(method.isSynthetic() && method.getName().equals("invoke"));
                check(method.getReturnType() == Object.class);
                ++bridges;
            }
            if (method.getName().equals("invoke") && method.getReturnType() == kotlin.Unit.class) ++typed;
        }
        check(voids == 1 && bridges == 1);
        check(typed == (name.equals("invoke") ? 0 : 1));
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Exception {
        Worker worker = new Worker();
        Action action = worker;
        check(Trace.events == 0);
        Worker.call(worker);
        check(Trace.events == 1 && worker.calls == 1);
        RuntimeException sentinel = new IllegalStateException();
        worker.failure = sentinel;
        try {
            Worker.call(worker);
            throw new AssertionError();
        } catch (RuntimeException actual) {
            same(actual, sentinel);
            ++failures;
        }
        check(Trace.events == 11 && worker.calls == 2);
        try {
            action.invoke();
            throw new AssertionError();
        } catch (RuntimeException actual) {
            same(actual, sentinel);
            ++failures;
        }
        check(Trace.events == 111 && worker.calls == 3);
        worker.failure = null;
        Object first = action.invoke();
        check(Trace.events == 11112 && worker.calls == 4);
        same(first, kotlin.Unit.INSTANCE);
        Worker.call(worker);
        check(Trace.events == 111121 && worker.calls == 5);
        same(action.invoke(), first);
        check(Trace.events == 1111211 && worker.calls == 6);
        Worker.class.getMethod(args[0]).invoke(worker);
        check(Trace.events == 11112111 && worker.calls == 7);
        ArgWorker argWorker = new ArgWorker();
        ArgAction raw = argWorker;
        Object payload = new Object();
        Value a = new Value(payload);
        Value b = new Value(new Object());
        for (Value value : new Value[] {a, b, null, a}) {
            int before = argWorker.calls;
            same(raw.invoke(value), first);
            same(argWorker.value, value);
            check(argWorker.calls == before + 1);
            ArgWorker.call(argWorker, value);
            same(argWorker.value, value);
            check(argWorker.calls == before + 2);
        }
        a.payload = first;
        same(((Value) argWorker.value).payload, first);
        argWorker.failure = sentinel;
        for (Object invalid : new Object[] {payload, "wrong", Integer.valueOf(5)}) {
            int before = argWorker.calls;
            Object previous = argWorker.value;
            try {
                raw.invoke(invalid);
                throw new AssertionError();
            } catch (ClassCastException expected) {
                check(argWorker.calls == before);
                same(argWorker.value, previous);
                ++failures;
            }
        }
        for (Value value : new Value[] {a, b, null}) {
            int before = argWorker.calls;
            try {
                raw.invoke(value);
                throw new AssertionError();
            } catch (RuntimeException actual) {
                same(actual, sentinel);
                same(argWorker.value, value);
                check(argWorker.calls == before + 1);
                ++failures;
            }
        }
        try {
            ((Action) null).invoke();
            throw new AssertionError();
        } catch (NullPointerException expected) { ++failures; }
        argWorker.failure = null;
        ArgWorker.class.getMethod(args[1], Value.class).invoke(argWorker, a);
        same(argWorker.value, a);
        reflected(Worker.class, args[0], null);
        reflected(ArgWorker.class, args[1], Value.class);
        check(Worker.class.getGenericInterfaces()[0].getTypeName().equals("fixture.Action<kotlin.Unit>"));
        check(ArgWorker.class.getGenericInterfaces()[0].getTypeName().equals("fixture.ArgAction<fixture.Value, kotlin.Unit>"));
        System.out.println("identities=" + identities + ",failures=" + failures + ",initialization-and-calls=matched");
        System.out.println("reflection=" + (args[0].equals("invoke") ? "original" : "reported-source-mapping"));
    }
}
