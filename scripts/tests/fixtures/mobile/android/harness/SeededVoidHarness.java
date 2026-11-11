import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class SeededVoidHarness {
    static final long FLAGS = Long.MIN_VALUE + 37;
    static void require(boolean value) { if (!value) throw new AssertionError("seeded void callback changed"); }
    static void call(Object seed, Fn<?,?> callback, Factory<?> task, int route) throws Throwable {
        if (route == 0) { Client.call(seed, callback, task, FLAGS); return; }
        if (route == 1) { SeededVoidCaller.call(seed, callback, task, FLAGS); return; }
        if (route == 2) {
            try { Client.class.getMethod("call", Object.class, Fn.class, Factory.class, long.class).invoke(null, seed, callback, task, FLAGS); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
            return;
        }
        MethodHandles.publicLookup().findStatic(Client.class, "call",
                MethodType.methodType(void.class, Object.class, Fn.class, Factory.class, long.class)).invokeExact(seed, callback, task, FLAGS);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        Trace.failAt = mode >= 1 && mode <= 6 ? mode : 0;
        Trace.failure = new RuntimeException("owned seeded void failure");
        int[] input = {7}, callbacks = {0}, tasks = {0};
        Object seed = mode == 7 ? null : mode == 14 ? new Object() : input;
        Object saved = mode == 10 ? new Object() : mode == 12 ? null : new String("saved");
        Object result = mode == 11 ? new Object() : mode == 13 ? null : new String("task");
        Fn<int[],Object> callback = mode == 8 ? null : values -> {
            require(values == seed); callbacks[0]++; Trace.hit(3);
            if (values != null) values[0]++;
            return saved;
        };
        Factory<Object> task = mode == 9 ? null : () -> {
            tasks[0]++; Trace.hit(5); return result;
        };
        Object sentinel = new Object();
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.events = 0; Trace.saved = sentinel; Trace.taskResult = sentinel;
            int oldCallbacks = callbacks[0], oldTasks = tasks[0], oldInput = input[0];
            Throwable failure = null;
            try { call(seed, callback, task, route); } catch (Throwable error) { failure = error; }
            boolean enters = mode != 1 && mode != 2 && mode != 8 && mode != 14;
            boolean firstResult = enters && mode != 3 && mode != 10;
            boolean taskEnters = firstResult && mode != 4 && mode != 9;
            boolean taskResult = taskEnters && mode != 5 && mode != 11;
            int trace = attempt == 0 ? 1 : 0;
            if (mode != 1) trace = trace * 10 + 2;
            if (enters) trace = trace * 10 + 3;
            if (firstResult) trace = trace * 10 + 4;
            if (taskEnters) trace = trace * 10 + 5;
            if (taskResult) trace = trace * 10 + 6;
            require(Trace.events == trace && callbacks[0] == oldCallbacks + (enters ? 1 : 0));
            require(tasks[0] == oldTasks + (taskEnters ? 1 : 0));
            require(input[0] == oldInput + (enters && mode != 3 && mode != 7 ? 1 : 0));
            require(Trace.saved == (firstResult ? saved : sentinel));
            require(Trace.taskResult == (taskResult ? result : sentinel));
            if (mode != 1) require(Trace.flags == FLAGS);
            if (mode == 1) {
                if (attempt == 0) require(failure instanceof ExceptionInInitializerError && failure.getCause() == Trace.failure);
                else require(failure instanceof NoClassDefFoundError);
            } else if (mode >= 2 && mode <= 6) require(failure == Trace.failure);
            else if (mode == 8 || mode == 9) require(failure instanceof NullPointerException);
            else if (mode == 10 || mode == 11 || mode == 14) require(failure instanceof ClassCastException);
            else require(failure == null);
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
