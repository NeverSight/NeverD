import fixture.Core;
import fixture.Trace;
import java.util.ArrayList;
import java.util.Iterator;
import java.util.LinkedList;
import java.util.function.Function;

public final class IterableFunctionContextHarness {
    private static void require(boolean condition, String detail) {
        if (!condition) throw new AssertionError("owned iterable function context mismatch: " + detail);
    }
    private static final class State {
        final RuntimeException failure = new IllegalStateException("owned callback failure");
        final Error error = new AssertionError("owned callback error");
        final Object item = new Object(), mapped = new Object(), other = new Object();
        final ArrayList<Object> result = new ArrayList<Object>();
        final LinkedList<Object> replacement = new LinkedList<Object>();
        final Sequence sequence = new Sequence(this);
        Object sourceAfter, mapperAfter;
        Function<Object,Object> mapper;
        Runnable action;
        boolean flag, snapshot;
        int mode, iteratorCalls, nextCalls, hasNextCalls, mapperCalls, actionCalls, staleActions;
    }
    private static final class Sequence extends ArrayList<Object> {
        final State state;
        final Iterator<Object> cursor;
        Object current;
        Sequence(final State state) {
            this.state = state;
            current = state.item;
            add(current);
            cursor = new Iterator<Object>() {
                public boolean hasNext() { state.hasNextCalls++; return true; }
                public Object next() {
                    state.nextCalls++;
                    if (state.mode == 6) throw state.failure;
                    return current;
                }
                public void remove() { throw new UnsupportedOperationException(); }
            };
        }
        public Iterator<Object> iterator() {
            state.iteratorCalls++;
            if (state.mode == 5) throw state.failure;
            return state.mode == 7 ? null : cursor;
        }
    }
    private static void reset() {
        Trace.source = Trace.mapper = Trace.iterator = Trace.item = Trace.mapped = Trace.value = null;
        Trace.action = null;
        Trace.calls = Trace.relays = Trace.wides = Trace.stage = 0;
        Trace.flag = false;
        Trace.stamp = 0;
        Trace.ratio = 0;
    }
    private static Object call(int route, Iterable<Object> source, Function<Object,Object> mapper,
                               boolean flag, long stamp, double ratio) {
        switch (route) {
            case 0: return Core.raw(source, mapper, flag);
            case 1: return Core.narrow(source, mapper, flag);
            case 2: Core.ignored(source, mapper, flag); return null;
            case 3: return Core.relay(source, mapper);
            case 4: return Core.typed(source, mapper, flag);
            case 5: return Core.wide(source, mapper, flag, stamp, ratio);
            case 6: return Core.alias(source, mapper, flag);
            default: throw new AssertionError("owned route");
        }
    }
    private static void attempt(final int route, final int mode, int attempt) {
        reset();
        final State s = new State();
        s.mode = mode;
        s.flag = route == 3 ? false : ((mode + attempt) & 1) == 0;
        if (mode == 18) s.sequence.current = null;
        final Iterable<Object> source = mode == 4 ? null : s.sequence;
        final Runnable action = new Runnable() {
            public void run() {
                s.actionCalls++;
                require(Trace.stage == 3 && Trace.source == source && Trace.mapper == s.mapper
                        && Trace.flag == s.flag && Trace.iterator == s.sequence.cursor
                        && Trace.item == s.sequence.current && Trace.mapped == (mode == 11 ? null : s.mapped), "action snapshot");
                s.snapshot = true;
                if (mode == 14) throw s.failure;
                if (mode == 15) throw s.error;
                if (mode == 12) Trace.value = s.replacement;
                if (mode == 17) {
                    Trace.source = s.other;
                    Trace.mapper = s.other;
                    s.sequence.current = s.other;
                }
            }
        };
        s.action = action;
        s.mapper = mode == 8 ? null : new Function<Object,Object>() {
            public Object apply(Object value) {
                s.mapperCalls++;
                require(value == (mode == 18 ? null : s.item) && Trace.stage == 2
                        && Trace.source == source && Trace.mapper == s.mapper
                        && Trace.flag == s.flag && Trace.iterator == s.sequence.cursor
                        && Trace.item == value, "mapper snapshot");
                if (mode == 9) throw s.failure;
                if (mode == 10) throw s.error;
                if (mode == 16) Trace.action = action;
                return mode == 11 ? null : s.mapped;
            }
        };
        Trace.action = mode == 13 ? null : mode == 16 ? new Runnable() {
            public void run() { s.staleActions++; }
        } : action;
        Trace.value = mode == 1 ? s.sequence : mode == 2 ? null : mode == 3 ? s.other : s.result;
        Object wanted = mode == 12 ? s.replacement : Trace.value;
        s.sourceAfter = source;
        s.mapperAfter = s.mapper;
        long stamp = attempt == 0 ? 0x1020304050607080L : Long.MIN_VALUE + mode;
        double ratio = attempt == 0 ? -0.0 : Double.longBitsToDouble(0x7ff8000000000013L);
        Object observed = s.other;
        Throwable thrown = null;
        try { observed = call(route, source, s.mapper, s.flag, stamp, ratio); }
        catch (Throwable failure) { thrown = failure; }
        Class<?> expectedClass = null;
        Throwable expectedIdentity = null;
        int stage = 4;
        if (mode == 4 || mode == 7) { expectedClass = NullPointerException.class; stage = 1; }
        else if (mode == 5 || mode == 6) { expectedIdentity = s.failure; stage = 1; }
        else if (mode == 8) { expectedClass = NullPointerException.class; stage = 2; }
        else if (mode == 9 || mode == 10) { expectedIdentity = mode == 9 ? s.failure : s.error; stage = 2; }
        else if (mode == 13) { expectedClass = NullPointerException.class; stage = 3; }
        else if (mode == 14 || mode == 15) { expectedIdentity = mode == 14 ? s.failure : s.error; stage = 3; }
        else if (mode == 3 || (route == 1 && mode == 12)) expectedClass = ClassCastException.class;
        if (expectedIdentity != null) require(thrown == expectedIdentity, "exact failure identity");
        else if (expectedClass != null) require(thrown != null && thrown.getClass() == expectedClass, "exact failure class");
        else require(thrown == null && observed == (route == 2 ? null : wanted), "actual returned reference");
        require(Trace.calls == 1 && Trace.relays == (route == 3 ? 1 : 0)
                && Trace.wides == (route == 5 ? 1 : 0) && Trace.stage == stage, "roles and stage");
        require(Trace.flag == s.flag, "original boolean");
        require(Trace.stamp == (route == 5 ? stamp : 0)
                && Double.doubleToRawLongBits(Trace.ratio) == Double.doubleToRawLongBits(route == 5 ? ratio : 0.0), "wide words");
        int iterators = mode == 4 ? 0 : 1;
        int next = mode == 4 || mode == 5 || mode == 7 ? 0 : 1;
        int maps = stage == 1 || mode == 8 ? 0 : 1;
        int actions = stage < 3 || mode == 13 ? 0 : 1;
        require(s.iteratorCalls == iterators && s.nextCalls == next && s.hasNextCalls == 0
                && s.mapperCalls == maps && s.actionCalls == actions && s.staleActions == 0, "exact effect counts");
        require(Trace.iterator == (mode == 4 || mode == 5 || mode == 7 ? null : s.sequence.cursor), "iterator identity");
        require(Trace.item == (stage == 1 ? null : mode == 18 ? null : s.item), "original item snapshot");
        require(Trace.mapped == (stage <= 2 || mode == 11 ? null : s.mapped), "mapped snapshot");
        require(s.snapshot == (actions != 0), "snapshot execution");
        boolean mutated = mode == 17;
        require(Trace.source == (mutated ? s.other : source) && Trace.mapper == (mutated ? s.other : s.mapper), "argument snapshots");
    }
    public static void main(String[] arguments) {
        int route = Integer.parseInt(arguments[0]), mode = Integer.parseInt(arguments[1]);
        for (int attempt = 0; attempt < 2; attempt++) attempt(route, mode, attempt);
        System.out.println("attempts=2,iterable-function-context=matched");
    }
}
