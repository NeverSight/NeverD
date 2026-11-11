import fixture.Core;
import fixture.Trace;
import java.util.AbstractCollection;
import java.util.Collection;
import java.util.Iterator;

public final class RepeatedArrayTokensHarness {
    private static void check(boolean value, String detail) {
        if (!value) throw new AssertionError("owned repeated array tokens mismatch: " + detail);
    }
    private static final class State {
        int mode, sizes, actions, stale;
        Class token;
        Runnable action;
        String[] late;
        final RuntimeException failure = new IllegalStateException("owned runtime");
        final Error error = new LinkageError("owned error");
    }
    private static final class Source extends AbstractCollection<Object> {
        final State state;
        Source(State state) { this.state = state; }
        public int size() {
            state.sizes++;
            check(Trace.calls == 1 && Trace.stage == 1 && Trace.source == this
                    && Trace.token == state.token && state.sizes == 1, "original collection dispatch");
            check(Trace.name.equals(state.token.getName()), "token before collection");
            if (state.mode == 4) throw state.failure;
            if (state.mode == 5) throw state.error;
            if (state.mode == 14) Trace.action = state.action;
            if (state.mode == 17) Trace.value = state.late;
            return state.mode == 21 ? -7 : state.mode == 19 ? 0 : 2;
        }
        public Iterator<Object> iterator() { throw new AssertionError("owned collection traversed"); }
        public Object[] toArray() { throw new AssertionError("owned collection converted"); }
        public <T> T[] toArray(T[] array) { throw new AssertionError("owned collection converted"); }
    }
    private static Object call(int route, Collection source, Class token) {
        switch (route) {
            case 0: return Core.raw(source,token);
            case 1: return Core.narrow(source,token);
            case 2: return Core.ignored(source,token);
            case 3: return Core.reverse(source,token);
            case 4: return Core.typed(source,token);
            case 5: return Core.alias(source,token);
            default: throw new AssertionError("owned route");
        }
    }
    private static void attempt(final int route, final int mode, int attempt) {
        final State s = new State();s.mode = mode;
        s.token = mode == 2 || mode == 3 ? null : mode == 18 ? Integer.class : String.class;
        final Source original = new Source(s);
        final Collection source = mode == 1 || mode == 3 ? null : original;
        final Class token = s.token;
        final Object poison = new Object(), marker = new Object();
        final String replacement = new String("owned replacement");
        final String[] array = new String[]{new String("owned first"),new String("owned second")};
        s.late = new String[]{new String("owned late")};
        Trace.calls = Trace.relays = Trace.stage = Trace.size = 0;
        Trace.source = Trace.token = null;Trace.name = null;Trace.marker = marker;
        Trace.value = mode == 9 ? null : mode == 10 ? poison : mode == 11 ? new int[]{1}
                : mode == 12 ? new Object[]{poison} : mode == 13 ? new String[][]{array} : array;
        final Object initial = Trace.value;
        s.action = new Runnable() {
            public void run() {
                s.actions++;
                check(Trace.calls == 1 && Trace.stage == 2 && s.sizes == 1
                        && Trace.source == source && Trace.token == token, "callback order and input identities");
                check(Trace.name.equals(token.getName())
                        && Trace.size == (mode == 21 ? -7 : mode == 19 ? 0 : 2), "name and size snapshots");
                if (mode == 7) throw s.failure;
                if (mode == 8) throw s.error;
                if (mode == 15) Trace.value = s.late;
                if (mode == 16) { Trace.source = poison; Trace.token = poison; }
                if (mode == 20) array[0] = replacement;
            }
        };
        Trace.action = mode == 6 ? null : mode == 14 ? new Runnable() {
            public void run() { s.stale++; }
        } : s.action;
        Object observed = null;Throwable problem = null;
        try { observed = call(route,source,token); } catch (Throwable caught) { problem = caught; }
        int stage = 3;Class<?> kind = null;Throwable exact = null;
        if (mode == 1 || mode == 2 || mode == 3) { stage = 1;kind = NullPointerException.class; }
        else if (mode == 4 || mode == 5) { stage = 1;exact = mode == 4 ? s.failure : s.error; }
        else if (mode == 6) { stage = 2;kind = NullPointerException.class; }
        else if (mode == 7 || mode == 8) { stage = 2;exact = mode == 7 ? s.failure : s.error; }
        else if (mode == 10 || mode == 11 || route == 1 && (mode == 12 || mode == 13)) kind = ClassCastException.class;
        if (exact != null) check(problem == exact,"failure identity");
        else if (kind != null) check(problem != null && problem.getClass() == kind,"failure class and cast order");
        else check(problem == null && observed == (route == 2 ? marker
                : mode == 15 || mode == 17 ? s.late : initial),"actual array reference");
        check(Trace.calls == 1 && Trace.relays == (route == 3 ? 1 : 0) && Trace.stage == stage,"one original provider");
        int sizes = mode == 1 || mode == 2 || mode == 3 ? 0 : 1;
        check(s.sizes == sizes && s.actions == (stage < 2 || mode == 6 ? 0 : 1)
                && s.stale == 0,"exact effect counts");
        check(Trace.name == null ? token == null : Trace.name.equals(token.getName()),"name before null or failed collection");
        check(Trace.size == (stage == 1 ? 0 : mode == 21 ? -7 : mode == 19 ? 0 : 2),"size assignment boundary");
        check(Trace.source == (mode == 16 ? poison : source)
                && Trace.token == (mode == 16 ? poison : token),"argument snapshot and later writes");
        if (mode == 20) check(array[0] == replacement,"original result mutation");
    }
    public static void main(String[] args) {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        for (int attempt = 0; attempt < 2; attempt++) attempt(route,mode,attempt);
        System.out.println("attempts=2,repeated-array-tokens=matched");
    }
}
