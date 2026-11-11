import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
import java.util.*;
public final class RecursiveVoidCarrierHarness {
    static void require(boolean value) { if (!value) throw new AssertionError("recursive void carrier contract"); }
    static void call(int entry, int route, List values) throws Throwable {
        if (route == 0) {
            if (entry == 0) Core.create(values);
            else if (entry == 1) Core.guarded(values);
            else Core.locked(values);
        } else if (route == 1) RecursiveVoidCarrierCaller.call(entry, values);
        else {
            String name = entry == 0 ? "create" : entry == 1 ? "guarded" : "locked";
            if (route == 2) {
                try { Core.class.getMethod(name, List.class).invoke(null, values); }
                catch (InvocationTargetException failure) { throw failure.getCause(); }
            } else MethodHandles.publicLookup().findStatic(Core.class, name,
                    MethodType.methodType(void.class, List.class)).invokeExact(values);
        }
    }
    public static void main(String[] args) throws Throwable {
        int entry = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        Trace.lock = new Object(); Trace.expectLock = entry == 2;
        Trace.failAt = mode <= 4 ? mode : 0;
        Trace.failure = new IllegalArgumentException("owned recursive carrier failure");
        for (int attempt = 0; attempt < 2; attempt++) {
            Item first = new Item(1, 1), second = new Item(2, 2), third = new Item(3, 3);
            List values = new ArrayList(Arrays.asList(third, first, second));
            if (mode == 5) values = null;
            else if (mode == 6) values.set(1, new Object());
            else if (mode == 7) values.set(1, null);
            else if (mode == 8) values = Collections.unmodifiableList(values);
            else if (mode == 9) values = new ArrayList(Arrays.asList(first, second, third));
            Trace.events = Trace.comparisons = 0; Trace.pairs = 0;
            Trace.seen = Trace.finished = null; Trace.last = null;
            Throwable thrown = null;
            try { call(entry, route, values); } catch (Throwable failure) { thrown = failure; }
            require(!Thread.holdsLock(Trace.lock));
            Throwable observed = entry == 1 ? Trace.last : thrown;
            require(entry != 1 || thrown == null);
            int comparisons = mode == 2 ? 1 : mode == 0 || mode == 3 ? 4 : mode == 9 ? 2 : 0;
            long pairs = mode == 2 ? 13 : mode == 0 || mode == 3 ? 13212321L : mode == 9 ? 2132 : 0;
            int events = attempt == 0 ? 4 : 0;
            if (mode != 4) events = events * 10 + 1;
            for (int i = 0; i < comparisons; i++) events = events * 10 + 2;
            if (mode == 0 || mode == 3 || mode == 9) events = events * 10 + 3;
            require(Trace.events == events && Trace.comparisons == comparisons && Trace.pairs == pairs);
            require(Trace.seen == (mode == 1 || mode == 4 ? null : values));
            require(Trace.finished == (mode == 0 || mode == 9 ? values : null));
            if (mode == 0 || mode == 9) require(observed == null);
            else if (mode == 1 || mode == 2 || mode == 3) require(observed == Trace.failure);
            else if (mode == 4) {
                if (attempt == 0) require(observed instanceof ExceptionInInitializerError && observed.getCause() == Trace.failure);
                else require(observed instanceof NoClassDefFoundError);
            } else if (mode == 6) require(observed instanceof ClassCastException);
            else if (mode == 8) require(observed instanceof UnsupportedOperationException);
            else require(observed instanceof NullPointerException);
            if (mode == 0 || mode == 3 || mode == 9)
                require(values.get(0) == first && values.get(1) == second && values.get(2) == third);
            else if (mode != 5 && mode != 9) require(values.get(0) == third && values.get(2) == second);
            System.out.println("entry=" + entry + ",route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
