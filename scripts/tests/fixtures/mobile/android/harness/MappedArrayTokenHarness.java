import fixture.Core;
import fixture.Trace;
import java.util.function.Function;

public final class MappedArrayTokenHarness {
    private static void require(boolean condition, String detail) {
        if (!condition) throw new AssertionError("owned mapped array token mismatch: " + detail);
    }
    private static final class State {
        final Object item = new Object(), mapped = new Object(), other = new Object();
        final Object[] alias = new Object[]{item};
        final String[] result = new String[]{"owned"};
        final Object[] replacement = new Object[]{other};
        final RuntimeException failure = new IllegalStateException("owned callback failure");
        final Error error = new AssertionError("owned callback error");
        Object source;
        Class<Object> token;
        Function<Object,Object> mapper;
        int maps, actions, stale;
        boolean snapshot;
    }
    private static Object call(int route, State s) {
        switch (route) {
            case 0: return Core.raw(s.source, s.token, s.mapper);
            case 1: return Core.narrow(s.source, s.token, s.mapper);
            case 2: return Core.ignored(s.source, s.token, s.mapper);
            case 3: return Core.relay(s.source, s.token, s.mapper);
            case 4: return Core.typed(s.source, s.token, s.mapper);
            case 5: return Core.token(s.source, s.token, s.mapper);
            case 6: return Core.alias(s.source, s.token, s.mapper);
            default: throw new AssertionError("owned route");
        }
    }
    @SuppressWarnings("unchecked")
    private static void attempt(final int route, final int mode, int attempt) {
        Trace.source = Trace.token = Trace.mapper = Trace.mapped = Trace.value = Trace.marker = null;
        Trace.name = null;
        Trace.action = null;
        Trace.calls = Trace.relays = Trace.stage = 0;
        final State s = new State();
        s.source = mode == 1 ? s.alias : mode == 5 ? null : s.item;
        Class<?> token = mode == 19 ? int.class : attempt == 0 ? String.class : Object.class;
        s.token = mode == 4 ? null : (Class<Object>)token;
        final Runnable action = new Runnable() {
            public void run() {
                s.actions++;
                require(Trace.stage == 3 && Trace.source == s.source && Trace.mapper == s.mapper
                        && Trace.token == (mode == 15 ? Integer.class : s.token)
                        && Trace.name.equals(s.token.getName()) && Trace.mapped == (mode == 9 ? null : s.mapped), "action snapshot");
                s.snapshot = true;
                if (mode == 13) throw s.failure;
                if (mode == 14) throw s.error;
                if (mode == 12) Trace.value = s.replacement;
                if (mode == 16) { Trace.source = s.other; Trace.mapper = s.other; }
            }
        };
        s.mapper = mode == 6 ? null : new Function<Object,Object>() {
            public Object apply(Object value) {
                s.maps++;
                require(value == s.source && Trace.stage == 2 && Trace.source == s.source
                        && Trace.mapper == s.mapper && Trace.token == s.token
                        && Trace.name.equals(s.token.getName()), "mapper snapshot");
                if (mode == 7) throw s.failure;
                if (mode == 8) throw s.error;
                if (mode == 10) Trace.action = action;
                if (mode == 15) Trace.token = Integer.class;
                return mode == 9 ? null : s.mapped;
            }
        };
        Trace.value = mode == 1 ? s.alias : mode == 2 ? null : mode == 3 ? s.other
                : mode == 17 ? new int[]{attempt} : mode == 18 ? new String[][]{{"owned"}} : s.result;
        Object wanted = mode == 12 ? s.replacement : Trace.value;
        Trace.marker = s.other;
        Trace.action = mode == 11 ? null : mode == 10 ? new Runnable() {
            public void run() { s.stale++; }
        } : action;
        Object observed = s.item;
        Throwable thrown = null;
        try { observed = call(route, s); }
        catch (Throwable failure) { thrown = failure; }
        Class<?> expectedClass = null;
        Throwable expectedIdentity = null;
        int stage = 4;
        if (mode == 4) { expectedClass = NullPointerException.class; stage = 1; }
        else if (mode == 6) { expectedClass = NullPointerException.class; stage = 2; }
        else if (mode == 7 || mode == 8) { expectedIdentity = mode == 7 ? s.failure : s.error; stage = 2; }
        else if (mode == 11) { expectedClass = NullPointerException.class; stage = 3; }
        else if (mode == 13 || mode == 14) { expectedIdentity = mode == 13 ? s.failure : s.error; stage = 3; }
        else if (mode == 3 || mode == 17 || (route == 1 && (mode == 1 || mode == 12 || mode == 18))) expectedClass = ClassCastException.class;
        if (expectedIdentity != null) require(thrown == expectedIdentity, "exact failure identity");
        else if (expectedClass != null) require(thrown != null && thrown.getClass() == expectedClass, "exact failure class");
        else require(thrown == null && observed == (route == 2 ? s.other : wanted), "actual array/ignored reference");
        require(Trace.calls == 1 && Trace.relays == (route == 3 ? 1 : 0) && Trace.stage == stage, "calls and stage");
        int maps = mode == 4 || mode == 6 ? 0 : 1;
        int actions = stage < 3 || mode == 11 ? 0 : 1;
        require(s.maps == maps && s.actions == actions && s.stale == 0 && s.snapshot == (actions != 0), "effect counts");
        require(Trace.mapped == (stage <= 2 || mode == 9 ? null : s.mapped), "mapped snapshot");
        require(Trace.token == (mode == 15 ? Integer.class : s.token), "token identity");
        require(mode == 4 ? Trace.name == null : Trace.name.equals(s.token.getName()), "original token name");
        require(Trace.source == (mode == 16 ? s.other : s.source) && Trace.mapper == (mode == 16 ? s.other : s.mapper), "argument snapshots");
    }
    public static void main(String[] arguments) {
        int route = Integer.parseInt(arguments[0]), mode = Integer.parseInt(arguments[1]);
        for (int attempt = 0; attempt < 2; attempt++) attempt(route, mode, attempt);
        System.out.println("attempts=2,mapped-array-token=matched");
    }
}
