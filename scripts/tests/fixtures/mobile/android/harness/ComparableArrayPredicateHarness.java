package fixture;
import java.io.IOException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;

public final class ComparableArrayPredicateHarness {
    private static void require(boolean ok) {
        if (!ok) throw new AssertionError("owned comparable array predicate mismatch");
    }
    @SuppressWarnings("unchecked") private static <X extends Throwable> void fail(Throwable failure) throws X { throw (X) failure; }
    public static class Base implements Comparable<Object> {
        final State state;
        Base(State state) { this.state = state; }
        public int compareTo(Object other) {
            require(other == this && state.comparisons++ == 0 && Trace.events == 12 && Trace.values == state.values && Trace.result == state.boxed);
            if (state.mode >= 13 && state.mode <= 15) ComparableArrayPredicateHarness.<RuntimeException>fail(state.failure);
            if (state.mode == 12) state.values[0] = state.alternate;
            return state.comparison;
        }
    }
    public static final class Leaf extends Base { Leaf(State state) { super(state); } }
    public static final class Strict implements Comparable<Integer> {
        public int compareTo(Integer other) { throw new AssertionError("owned strict bridge was bypassed"); }
    }
    private static final class State {
        final int mode, comparison;
        final boolean desired;
        final Object sentinel = new Object(), boxed;
        final Leaf leaf = new Leaf(this), alternate = new Leaf(this);
        final Base other = new Base(this);
        final Comparable[] values;
        final Throwable failure;
        int actions, comparisons;
        State(int mode, int attempt) {
            this.mode = mode; desired = mode != 11 && attempt == 0;
            comparison = attempt == 0 ? Integer.MIN_VALUE + 17 : Integer.MAX_VALUE - 29;
            boxed = mode == 9 ? null : mode == 10 ? new Object() : Boolean.valueOf(desired);
            values = mode == 1 || mode == 16 ? null : mode == 2 || mode == 17 ? new Leaf[0]
                    : mode == 3 ? new Comparable[]{null} : mode == 18 ? new Comparable[]{new Strict()} : new Leaf[]{leaf};
            failure = mode == 4 || mode == 13 ? new IllegalStateException("owned runtime")
                    : mode == 5 || mode == 14 ? new AssertionError("owned error") : new IOException("owned checked");
        }
    }
    private static boolean call(int route, int operation, Object values) throws Throwable {
        if (route == 0) return operation == 0 ? Core.call((Comparable[]) values) : Core.checked(values);
        if (route == 1) return operation == 0 ? ComparableArrayPredicateCaller.call((Comparable[]) values) : ComparableArrayPredicateCaller.checked(values);
        if (route == 2) {
            try { return ((Boolean) Core.class.getMethod(operation == 0 ? "call" : "checked", operation == 0 ? Comparable[].class : Object.class).invoke(null, values)).booleanValue(); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        if (operation == 0) return (boolean) MethodHandles.publicLookup().findStatic(Core.class, "call",
                MethodType.methodType(boolean.class, Comparable[].class)).invokeExact((Comparable[]) values);
        return (boolean) MethodHandles.publicLookup().findStatic(Core.class, "checked",
                MethodType.methodType(boolean.class, Object.class)).invokeExact(values);
    }
    public static void main(String[] args) throws Throwable {
        final int route = Integer.parseInt(args[0]), operation = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        for (int attempt = 0; attempt < 2; ++attempt) {
            final State state = new State(mode, attempt);
            final Object input = mode == 8 && operation == 1 ? new Object() : state.values;
            Trace.events = 0; Trace.comparison = 41; Trace.values = null; Trace.result = state.sentinel;
            Trace.read = mode != 1 && mode != 2 && mode != 19;
            Trace.action = new Runnable() { public void run() {
                require(state.actions++ == 0 && Trace.events == 1 && Trace.values == state.values && Trace.result == state.sentinel && Trace.comparison == 41);
                Trace.result = state.boxed;
                if (mode >= 4 && mode <= 6) ComparableArrayPredicateHarness.<RuntimeException>fail(state.failure);
                if (state.values != null && state.values.length != 0 && mode != 3 && mode != 18)
                    state.values[0] = mode == 7 ? state.other : state.leaf;
            } };
            boolean result = !state.desired; Throwable caught = null;
            try { result = call(route, operation, input); } catch (Throwable failure) { caught = failure; }
            final boolean badCast = mode == 8 && operation == 1;
            final boolean actionFailure = mode >= 4 && mode <= 7;
            final boolean readFailure = mode == 3 || mode >= 13 && mode <= 18;
            final boolean lateFailure = mode == 9 || mode == 10;
            final boolean compared = !badCast && !actionFailure && mode != 3 && mode != 16 && mode != 17 && mode != 18 && Trace.read;
            final boolean finishedComparison = compared && !(mode >= 13 && mode <= 15);
            require(state.actions == (badCast ? 0 : 1) && state.comparisons == (compared ? 1 : 0));
            require(Trace.events == (badCast ? 0 : actionFailure ? 1 : readFailure ? 12 : 123));
            require(Trace.comparison == (finishedComparison ? state.comparison : 41));
            if (badCast) require(caught != null && caught.getClass() == ClassCastException.class);
            else if (mode >= 4 && mode <= 6 || mode >= 13 && mode <= 15) require(caught == state.failure);
            else if (mode == 7) require(caught != null && caught.getClass() == ArrayStoreException.class);
            else if (mode == 3 || mode == 9 || mode == 16) require(caught != null && caught.getClass() == NullPointerException.class);
            else if (mode == 10 || mode == 18) require(caught != null && caught.getClass() == ClassCastException.class);
            else if (mode == 17) require(caught != null && caught.getClass() == ArrayIndexOutOfBoundsException.class);
            else require(caught == null && result == state.desired);
            if (badCast || actionFailure || readFailure || lateFailure) require(result == !state.desired);
            require(Trace.values == (badCast ? null : state.values) && Trace.result == (badCast ? state.sentinel : state.boxed));
            if (mode == 12) require(state.values[0] == state.alternate);
            if (mode == 7) require(state.values[0] == state.leaf);
        }
        System.out.println("attempts=2,comparable-array-predicate=matched");
    }
}
