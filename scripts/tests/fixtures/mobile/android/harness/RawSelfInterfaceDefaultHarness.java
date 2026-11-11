import fixture.*;
import java.lang.reflect.*;

public final class RawSelfInterfaceDefaultHarness {
    private static int values, identities, states, failures;
    private interface Call { int run(Object other) throws Throwable; }
    private static class Entry implements Self<String> {
        final int score;
        final Object payload;
        RuntimeException problem;
        Entry(int score, Object payload) { this.score = score; this.payload = payload; }
        public int rank() {
            Trace.ranks++;
            if (problem != null) throw problem;
            return score;
        }
    }
    private static final class Leaf extends Entry {
        Leaf(int score, Object payload) { super(score, payload); }
        public int compareTo(Self other) { Trace.calls += 2; return super.compareTo(other); }
    }
    private static int invoke(Method method, Object receiver, Object arg) throws Throwable {
        try { return ((Integer) method.invoke(receiver, arg)).intValue(); }
        catch (InvocationTargetException error) { throw error.getCause(); }
    }
    private static void same(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("original raw default identity changed");
        identities++;
    }
    private static void reset(Entry receiver, RuntimeException failure, RuntimeException rankFailure) {
        Trace.calls = 0; Trace.ranks = 0; Trace.seen = null;
        Trace.failure = failure; receiver.problem = rankFailure;
    }
    private static void state(Entry receiver, int calls, int ranks, Object seen,
                              RuntimeException failure, Object payload) {
        if (Trace.calls != calls || Trace.ranks != ranks)
            throw new AssertionError("original default effects or dispatch changed");
        same(Trace.seen, seen); same(Trace.failure, failure); same(receiver.payload, payload);
        states++;
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Throwable {
        Method typed = Self.class.getDeclaredMethod("compareTo", Self.class);
        Method bridge = Self.class.getDeclaredMethod("compareTo", Object.class);
        if (typed.isBridge() || typed.getGenericParameterTypes()[0] != Self.class
                || !typed.isDefault() || !bridge.isDefault() || !bridge.isBridge() || !bridge.isSynthetic()
                || typed.getTypeParameters().length != 0 || bridge.getTypeParameters().length != 0
                || typed.getExceptionTypes().length != 0 || bridge.getExceptionTypes().length != 0
                || Self.class.getTypeParameters().length != 1
                || !Self.class.getGenericInterfaces()[0].getTypeName()
                    .equals("java.lang.Comparable<fixture.Self<T>>"))
            throw new AssertionError("original raw parameter or scoped interface metadata changed");
        Object payload = new Object();
        Entry other = new Entry(4, payload);
        for (Entry receiver : new Entry[] {new Entry(9, payload), new Leaf(13, payload)}) {
            int delta = receiver instanceof Leaf ? 3 : 1;
            Comparable<Self<String>> view = receiver;
            Comparable raw = receiver;
            Call[] paths = {arg -> receiver.compareTo((Self) arg),
                            arg -> view.compareTo((Self<String>) arg), raw::compareTo,
                            arg -> invoke(typed, receiver, arg), arg -> invoke(bridge, receiver, arg)};
            for (int route = 0; route < paths.length; route++) {
                Call path = paths[route];
                for (Entry value : new Entry[] {receiver, other, null}) {
                    reset(receiver, null, null);
                    int actual = path.run(value);
                    int expected = value == null ? receiver.score : receiver.score - value.score;
                    if (actual != expected) throw new AssertionError("original raw comparison result changed");
                    state(receiver, delta, value == null ? 1 : 2, value, null, payload);
                    if (value != null) same(value.payload, payload);
                    values++;
                }
                RuntimeException problem = new IllegalStateException("owned default failure");
                reset(receiver, problem, null);
                try { path.run(other); throw new AssertionError("default failure missing"); }
                catch (Throwable error) { same(error, problem); failures++; }
                state(receiver, delta, 0, other, problem, payload);

                problem = new IllegalArgumentException("owned rank failure");
                reset(receiver, null, problem);
                try { path.run(other); throw new AssertionError("rank failure missing"); }
                catch (Throwable error) { same(error, problem); failures++; }
                state(receiver, delta, 1, other, null, payload);

                reset(receiver, null, null);
                try { path.run(new Object()); throw new AssertionError("wrong argument failure missing"); }
                catch (Throwable error) {
                    same(error.getClass(), route == 3 ? IllegalArgumentException.class : ClassCastException.class);
                    failures++;
                }
                state(receiver, 0, 0, null, null, payload);
            }
            Call[] absent = {arg -> ((Self) null).compareTo((Self) arg),
                             arg -> ((Comparable<Self<String>>) null).compareTo((Self<String>) arg),
                             arg -> ((Comparable) null).compareTo(arg),
                             arg -> invoke(typed, null, arg), arg -> invoke(bridge, null, arg)};
            for (Call path : absent) {
                reset(receiver, null, null);
                try { path.run(null); throw new AssertionError("null receiver failure missing"); }
                catch (Throwable error) { same(error.getClass(), NullPointerException.class); failures++; }
                state(receiver, 0, 0, null, null, payload);
            }
        }
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("raw-self-parameter=original,scoped-comparable-parent=retained,original-default-and-checked-bridge=retained");
    }
}
