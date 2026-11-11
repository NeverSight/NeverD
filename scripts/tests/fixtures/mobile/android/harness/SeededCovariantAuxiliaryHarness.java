import fixture.Core;
import fixture.Provider;
import fixture.Trace;

public final class SeededCovariantAuxiliaryHarness {
    private static final class OverrideProvider extends Provider {
        @Override
        public <R> R read(R seed, Class<? extends Runnable> type) {
            Trace.overrides++;
            return super.read(seed, type);
        }
    }

    private static void check(boolean condition, String label) {
        if (!condition) throw new AssertionError("owned seeded covariant auxiliary mismatch: " + label);
    }

    private static void attempt(final int route, final int mode, final int attempt) {
        final boolean instance = route >= 4;
        final boolean noReceiver = instance && mode == 14;
        final Object seed = mode == 7 ? null : new Object();
        final Object initial = mode == 2 ? null : mode == 1 || mode == 16 && attempt == 0
                ? new Object() : new String("initial");
        final Object replacement = mode == 10 ? new String("replacement") : new Object();
        final Class<? extends Runnable> type = mode == 6 ? null : mode == 8 ? Thread.class : Runnable.class;
        final Provider receiver = mode == 14 ? null : mode == 15 ? new OverrideProvider() : new Provider();
        final RuntimeException callbackFailure = new IllegalStateException("callback");
        final Error callbackError = new LinkageError("callback");
        final Runnable action = mode == 3 ? null : new Runnable() {
            public void run() {
                Trace.callbacks++;
                if (mode == 4) throw callbackFailure;
                if (mode == 5) throw callbackError;
                if (mode == 9 || mode == 10 || mode == 17) Trace.value = replacement;
                if (mode == 11) Trace.value = null;
                if (mode == 12 || mode == 17) Trace.seed = replacement;
                if (mode == 13) Trace.type = Thread.class;
                if (mode == 17) throw callbackFailure;
            }
        };
        final Object unwritten = new Object();
        Trace.seed = unwritten;
        Trace.value = initial;
        Trace.type = null;
        Trace.name = "unwritten";
        Trace.action = action;
        Trace.receiver = null;
        Trace.calls = Trace.instances = Trace.overrides = Trace.callbacks = Trace.stage = 0;
        Object result = unwritten;
        Throwable thrown = null;
        try {
            switch (route) {
            case 0: result = Core.staticRaw(seed, type); break;
            case 1: result = Core.staticNarrow(seed, type); break;
            case 2: Core.staticIgnore(seed, type); break;
            case 3: result = Core.staticRelay(seed, type); break;
            case 4: result = Core.virtualRaw(receiver, seed, type); break;
            case 5: result = Core.virtualNarrow(receiver, seed, type); break;
            case 6: Core.virtualIgnore(receiver, seed, type); break;
            case 7: result = Core.virtualRelay(receiver, seed, type); break;
            default: throw new AssertionError("route");
            }
        } catch (Throwable failure) {
            thrown = failure;
        }
        final int callbacks = noReceiver || mode == 3 || mode == 6 ? 0 : 1;
        final int stage = noReceiver ? 0 : mode == 6 ? 1
                : mode == 3 || mode == 4 || mode == 5 || mode == 17 ? 2 : 3;
        final Object value = noReceiver ? initial : mode == 9 || mode == 10 || mode == 17
                ? replacement : mode == 11 ? null : initial;
        final Class<?> failure = noReceiver || mode == 3 || mode == 6 ? NullPointerException.class
                : mode == 4 || mode == 17 ? IllegalStateException.class : mode == 5 ? LinkageError.class
                : (route == 1 || route == 5) && value != null && !(value instanceof String)
                ? ClassCastException.class : null;
        check(Trace.calls == (noReceiver ? 0 : 1) && Trace.callbacks == callbacks && Trace.stage == stage, "ordered effects");
        check(Trace.instances == (instance && !noReceiver ? 1 : 0), "instance entry");
        check(Trace.overrides == (instance && mode == 15 ? 1 : 0), "virtual dispatch");
        check(Trace.receiver == (instance && !noReceiver ? receiver : null), "actual receiver");
        check(Trace.seed == (noReceiver ? unwritten : mode == 12 || mode == 17 ? replacement : seed), "seed identity");
        check(Trace.type == (noReceiver ? null : mode == 13 ? Thread.class : type), "token identity");
        check((stage >= 2 ? type.getName() : "unwritten").equals(Trace.name), "token read");
        check(Trace.value == value && Trace.action == action, "callback mutation");
        check(failure == null ? thrown == null : thrown != null && thrown.getClass() == failure, "failure kind");
        if (mode == 4 || mode == 17) check(thrown == callbackFailure, "exception identity");
        if (mode == 5) check(thrown == callbackError, "error identity");
        check(result == (failure == null && route != 2 && route != 6 ? value : unwritten), "actual result and late cast");
    }

    public static void main(String[] args) {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        for (int attempt = 0; attempt < 2; attempt++) attempt(route, mode, attempt);
        System.out.println("attempts=2,seeded-covariant-auxiliary=matched");
    }
}
