import fixture.Action;
import fixture.Callback;
import fixture.Trace;
import fixture.Worker;
import java.lang.reflect.Method;

@SuppressWarnings({"rawtypes", "unchecked"})
public class ParameterizedUnitBridgeHarness {
    static int identities;
    static int failures;
    static void check(boolean condition) {
        if (!condition) throw new AssertionError();
    }
    static void same(Object actual, Object expected) {
        check(actual == expected);
        ++identities;
    }
    static class Mutable implements Callback {
        Object answer;
        int reads;
        RuntimeException failure;
        public Object value() {
            ++reads;
            Trace.events = Trace.events * 10 + 3;
            if (failure != null) throw failure;
            return answer;
        }
    }
    public static void main(String[] args) throws Exception {
        Worker worker = new Worker();
        Mutable callback = new Mutable();
        callback.answer = Boolean.TRUE;
        Action raw = worker;
        RuntimeException sentinel = new IllegalStateException();
        check(Trace.events == 0);
        worker.failure = sentinel;
        try {
            Worker.call(worker, callback);
            throw new AssertionError();
        } catch (RuntimeException actual) {
            same(actual, sentinel);
            ++failures;
        }
        check(Trace.events == 1 && worker.calls == 1 && callback.reads == 0);
        try {
            raw.invoke("invalid");
            throw new AssertionError();
        } catch (ClassCastException expected) { ++failures; }
        check(Trace.events == 1 && worker.calls == 1);
        try {
            raw.invoke(callback);
            throw new AssertionError();
        } catch (RuntimeException actual) {
            same(actual, sentinel);
            ++failures;
        }
        check(Trace.events == 11 && worker.calls == 2 && callback.reads == 0);
        worker.failure = null;
        Worker.call(worker, callback);
        check(Trace.events == 1113 && worker.calls == 3 && callback.reads == 1 && worker.last);
        Object unit = raw.invoke(callback);
        check(Trace.events == 1113132 && worker.calls == 4 && callback.reads == 2);
        same(unit, kotlin.Unit.INSTANCE);
        for (Boolean value : new Boolean[] {Boolean.TRUE, Boolean.FALSE, null, Boolean.TRUE}) {
            callback.answer = value;
            int events = Trace.events;
            int calls = worker.calls;
            int reads = callback.reads;
            boolean previous = worker.last;
            if (value == null) {
                try {
                    raw.invoke(callback);
                    throw new AssertionError();
                } catch (NullPointerException expected) { ++failures; }
                check(worker.last == previous && Trace.events == events * 100 + 13
                        && worker.calls == calls + 1 && callback.reads == reads + 1);
            } else {
                same(raw.invoke(callback), unit);
                same(worker.callback, callback);
                check(worker.last == value.booleanValue());
                Worker.call(worker, callback);
                same(worker.callback, callback);
                check(worker.last == value.booleanValue() && worker.calls == calls + 2 && callback.reads == reads + 2
                        && Trace.events == (events * 100 + 13) * 100 + 13);
            }
        }
        callback.failure = sentinel;
        int calls = worker.calls;
        int reads = callback.reads;
        int events = Trace.events;
        try {
            raw.invoke(callback);
            throw new AssertionError();
        } catch (RuntimeException actual) {
            same(actual, sentinel);
            ++failures;
        }
        check(worker.calls == calls + 1 && callback.reads == reads + 1 && Trace.events == events * 100 + 13);
        callback.failure = null;
        for (Object invalid : new Object[] {new Object(), "invalid", Integer.valueOf(3)}) {
            callback.answer = invalid;
            calls = worker.calls;
            reads = callback.reads;
            events = Trace.events;
            boolean previous = worker.last;
            try {
                raw.invoke(callback);
                throw new AssertionError();
            } catch (ClassCastException expected) { ++failures; }
            same(worker.callback, callback);
            check(worker.calls == calls + 1 && callback.reads == reads + 1 && worker.last == previous
                    && Trace.events == events * 100 + 13);
        }
        for (int i = 0; i < 2; ++i) {
            calls = worker.calls;
            reads = callback.reads;
            events = Trace.events;
            try {
                if (i == 0) raw.invoke(null);
                else Worker.call(worker, null);
                throw new AssertionError();
            } catch (NullPointerException expected) { ++failures; }
            same(worker.callback, null);
            check(worker.calls == calls + 1 && callback.reads == reads && Trace.events == events * 10 + 1);
        }
        worker.failure = sentinel;
        try {
            raw.invoke(null);
            throw new AssertionError();
        } catch (RuntimeException actual) {
            same(actual, sentinel);
            ++failures;
        }
        calls = worker.calls;
        events = Trace.events;
        try {
            ((Action) null).invoke(callback);
            throw new AssertionError();
        } catch (NullPointerException expected) { ++failures; }
        check(worker.calls == calls && Trace.events == events);
        worker.failure = null;
        callback.answer = Boolean.FALSE;
        Method target = Worker.class.getDeclaredMethod(args[0], Callback.class);
        target.invoke(worker, callback);
        same(worker.callback, callback);
        check(!worker.last && worker.calls == calls + 1 && Trace.events == events * 100 + 13);
        check(target.getReturnType() == void.class && target.getGenericParameterTypes()[0].getTypeName()
                .equals("fixture.Callback<java.lang.Boolean>"));
        int bridges = 0;
        int wrappers = 0;
        for (Method method : Worker.class.getDeclaredMethods()) {
            if (method.isBridge()) {
                check(method.isSynthetic() && method.getName().equals("invoke") && method.getReturnType() == Object.class
                        && method.getParameterTypes()[0] == Object.class);
                ++bridges;
            }
            if (method.getName().equals("invoke") && method.getReturnType() == kotlin.Unit.class) {
                check(method.getGenericParameterTypes()[0] == Callback.class);
                ++wrappers;
            }
        }
        check(bridges == 1 && wrappers == (args[0].equals("invoke") ? 0 : 1));
        check(Worker.class.getGenericInterfaces()[0].getTypeName()
                .equals("fixture.Action<fixture.Callback<? extends java.lang.Boolean>, kotlin.Unit>"));
        System.out.println("identities=" + identities + ",failures=" + failures + ",bridge=1,ordered-initialization=matched");
        System.out.println("generic-void-target=retained,reflection="
                + (args[0].equals("invoke") ? "original" : "reported-erased-wrapper"));
    }
}
