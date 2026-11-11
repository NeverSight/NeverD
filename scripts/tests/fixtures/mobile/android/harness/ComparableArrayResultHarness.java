package fixture;
import java.io.IOException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.util.Comparator;

public final class ComparableArrayResultHarness {
    private static void require(boolean ok) {
        if (!ok) throw new AssertionError("owned comparable array result mismatch");
    }
    @SuppressWarnings("unchecked") private static <X extends Throwable> void fail(Throwable failure) throws X { throw (X) failure; }
    public static class Base implements Comparable<Object> {
        final State state;
        Base(State state) { this.state = state; }
        public int compareTo(Object other) { return state.compare(this, other, false); }
    }
    public static final class Leaf extends Base { Leaf(State state) { super(state); } }
    private static final class State implements Comparator<Object> {
        final int mode, operation, comparison;
        final Object sentinel = new Object();
        final Leaf initial = new Leaf(this), leaf = new Leaf(this), alternate = new Leaf(this), result = new Leaf(this), later = new Leaf(this);
        final Base other = new Base(this);
        final Comparable[] values;
        final Comparator comparator;
        final Throwable failure;
        Object output;
        int actions, comparisons;
        State(int operation, int mode, int attempt) {
            this.mode = mode; this.operation = operation;
            comparison = attempt == 0 ? Integer.MIN_VALUE + 17 : Integer.MAX_VALUE - 29;
            output = mode == 9 ? null : mode == 10 ? other : mode == 16 ? new Object() : result;
            values = mode == 1 ? null : mode == 2 ? new Leaf[0] : new Leaf[]{initial};
            comparator = operation == 0 || mode == 3 ? null : this;
            failure = mode == 4 || mode == 13 ? new IllegalStateException("owned runtime")
                    : mode == 5 || mode == 14 ? new AssertionError("owned error") : new IOException("owned checked");
        }
        public int compare(Object first, Object second) { return compare(first, second, true); }
        int compare(Object first, Object second, boolean custom) {
            require(first == leaf && second == leaf && custom == (comparator != null));
            require(comparisons++ == 0 && Trace.events == 12 && Trace.values == values && Trace.comparator == comparator && Trace.result == output);
            if (mode >= 13 && mode <= 15) ComparableArrayResultHarness.<RuntimeException>fail(failure);
            if (mode == 11) { values[0] = alternate; output = later; Trace.result = output; }
            return comparison;
        }
    }
    private static Object call(int route, int operation, Object values, Object comparator) throws Throwable {
        if (route == 0) {
            if (operation == 0) return Core.single((Comparable[]) values);
            if (operation == 1) return Core.configured((Comparable[]) values, (Comparator) comparator);
            return Core.checked(values, comparator);
        }
        if (route == 1) {
            if (operation == 0) return ComparableArrayResultCaller.single((Comparable[]) values);
            if (operation == 1) return ComparableArrayResultCaller.configured((Comparable[]) values, (Comparator) comparator);
            return ComparableArrayResultCaller.checked(values, comparator);
        }
        if (route == 2) {
            try {
                if (operation == 0) return Core.class.getMethod("single", Comparable[].class).invoke(null, values);
                if (operation == 1) return Core.class.getMethod("configured", Comparable[].class, Comparator.class).invoke(null, values, comparator);
                return Core.class.getMethod("checked", Object.class, Object.class).invoke(null, values, comparator);
            } catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        if (operation == 0) return (Comparable) MethodHandles.publicLookup().findStatic(Core.class, "single",
                MethodType.methodType(Comparable.class, Comparable[].class)).invokeExact((Comparable[]) values);
        if (operation == 1) return (Comparable) MethodHandles.publicLookup().findStatic(Core.class, "configured",
                MethodType.methodType(Comparable.class, Comparable[].class, Comparator.class)).invokeExact((Comparable[]) values, (Comparator) comparator);
        return (Comparable) MethodHandles.publicLookup().findStatic(Core.class, "checked",
                MethodType.methodType(Comparable.class, Object.class, Object.class)).invokeExact(values, comparator);
    }
    public static void main(String[] args) throws Throwable {
        final int route = Integer.parseInt(args[0]), operation = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        for (int attempt = 0; attempt < 2; ++attempt) {
            final State state = new State(operation, mode, attempt);
            final Object input = mode == 8 && operation == 2 ? new Object() : state.values;
            final Object callback = mode == 17 && operation == 2 ? new Object() : state.comparator;
            Trace.events = 0; Trace.comparison = 41; Trace.values = null; Trace.comparator = null; Trace.result = state.sentinel;
            Trace.invoke = mode != 1 && mode != 2 && mode != 12;
            Trace.action = new Runnable() { public void run() {
                require(state.actions++ == 0 && Trace.events == 1 && Trace.values == state.values && Trace.comparator == state.comparator && Trace.result == state.sentinel && Trace.comparison == 41);
                Trace.result = state.output;
                if (mode >= 4 && mode <= 6) ComparableArrayResultHarness.<RuntimeException>fail(state.failure);
                if (state.values != null && state.values.length != 0) state.values[0] = mode == 7 ? state.other : state.leaf;
            } };
            Object result = state.sentinel; Throwable caught = null;
            try { result = call(route, operation, input, callback); } catch (Throwable failure) { caught = failure; }
            final boolean badCast = operation == 2 && (mode == 8 || mode == 17);
            final boolean actionFailure = mode >= 4 && mode <= 7;
            final boolean compareFailure = mode >= 13 && mode <= 15;
            final boolean compared = !badCast && !actionFailure && Trace.invoke;
            require(state.actions == (badCast ? 0 : 1) && state.comparisons == (compared ? 1 : 0));
            require(Trace.events == (badCast ? 0 : actionFailure ? 1 : compareFailure ? 12 : 123));
            require(Trace.comparison == (compared && !compareFailure ? state.comparison : 41));
            if (badCast || mode == 16) require(caught != null && caught.getClass() == ClassCastException.class && result == state.sentinel);
            else if (mode >= 4 && mode <= 6 || compareFailure) require(caught == state.failure && result == state.sentinel);
            else if (mode == 7) require(caught != null && caught.getClass() == ArrayStoreException.class && result == state.sentinel);
            else require(caught == null && result == state.output);
            require(Trace.values == (badCast ? null : state.values) && Trace.comparator == (badCast ? null : state.comparator));
            require(Trace.result == (badCast ? state.sentinel : state.output));
            if (mode == 7) require(state.values[0] == state.initial);
            else if (!badCast && !actionFailure && state.values != null && state.values.length != 0) require(state.values[0] == (mode == 11 ? state.alternate : state.leaf));
            if (caught == null) {
                Throwable consumerFailure = null; State owner = null;
                try { owner = ((Leaf) result).state; } catch (Throwable failure) { consumerFailure = failure; }
                if (mode == 9) require(consumerFailure != null && consumerFailure.getClass() == NullPointerException.class);
                else if (mode == 10) require(consumerFailure != null && consumerFailure.getClass() == ClassCastException.class);
                else require(consumerFailure == null && owner == state);
            }
        }
        System.out.println("attempts=2,comparable-array-result=matched");
    }
}
