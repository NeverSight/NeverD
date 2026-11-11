import fixture.Core;
import fixture.Provider;
import fixture.Trace;
import java.lang.reflect.InvocationTargetException;

public final class StaticReifiableResultsHarness {
    private static void check(boolean condition, String label) {
        if (!condition) throw new AssertionError("owned static reifiable result mismatch: " + label);
    }

    private static void attempt(final int route, final int mode, final int attempt) throws Exception {
        final Object initial = mode == 2 ? null : mode == 1 ? new Object() : new String("initial");
        final Object replacement = mode == 10 || mode == 17 && attempt == 1
                ? new String("replacement") : new Object();
        final RuntimeException callbackFailure = new IllegalArgumentException("callback");
        final Error callbackError = new LinkageError("callback");
        final Class<?>[] tokens = mode == 6 ? null : mode == 7 ? new Class<?>[0]
                : mode == 16 ? new Class<?>[]{String.class, Long.class}
                : new Class<?>[]{mode == 8 ? null : String.class};
        final Runnable action = mode == 3 ? null : new Runnable() {
            public void run() {
                Trace.callbacks++;
                if (mode == 4) throw callbackFailure;
                if (mode == 5) throw callbackError;
                if (mode == 9 || mode == 10 || mode == 17) Trace.value = replacement;
                if (mode == 11) Trace.value = null;
                if (mode == 12) tokens[0] = Double.class;
                if (mode == 13) Trace.stamp = Long.MIN_VALUE + 19;
                if (mode == 15) ((Object[]) tokens)[0] = new Object();
            }
        };
        final String context = mode == 14 ? null : new String("context");
        final long stamp = attempt == 0 ? 0x1234567887654321L : -0x1234567887654321L;
        Trace.value = initial;
        Trace.context = context;
        Trace.argument = stamp;
        Trace.label = "unwritten";
        Trace.action = null;
        Trace.tokens = new Class<?>[]{Boolean.class};
        Trace.first = Integer.class;
        Trace.stamp = 0;
        Trace.calls = Trace.callbacks = Trace.relays = Trace.stage = 0;
        Trace.length = -1;
        final Object ignored = new Object();
        Object result = ignored;
        Throwable thrown = null;
        try {
            switch (route) {
            case 0: result = Core.raw(action, tokens); break;
            case 1: result = Core.narrow(action, tokens); break;
            case 2: Core.ignore(action, tokens); break;
            case 3: result = Core.relay(action, tokens); break;
            case 4: result = Core.fixed(action, tokens); break;
            case 5: result = Provider.read(context, action, stamp, tokens); break;
            case 6:
                try {
                    result = Core.class.getMethod("relay", Runnable.class, Class[].class)
                            .invoke(null, new Object[]{action, tokens});
                } catch (InvocationTargetException failure) {
                    throw failure.getCause();
                }
                break;
            default: throw new AssertionError("route");
            }
        } catch (Throwable failure) {
            thrown = failure;
        }
        int stage = mode == 6 ? 1 : mode == 7 ? 2
                : mode == 3 || mode == 4 || mode == 5 || mode == 15 ? 3 : 4;
        int callbacks = mode == 3 || mode == 6 || mode == 7 ? 0 : 1;
        Object value = mode == 9 || mode == 10 || mode == 17 ? replacement
                : mode == 11 ? null : initial;
        Class<?> failure = mode == 3 || mode == 6 ? NullPointerException.class
                : mode == 7 ? ArrayIndexOutOfBoundsException.class
                : mode == 4 ? IllegalArgumentException.class : mode == 5 ? LinkageError.class
                : mode == 15 ? ArrayStoreException.class
                : route == 1 && value != null && !(value instanceof String) ? ClassCastException.class : null;
        check(Trace.calls == 1 && Trace.callbacks == callbacks && Trace.stage == stage, "ordered effects");
        check(Trace.relays == (route <= 3 || route == 6 ? 1 : 0), "overload forwarding");
        check(Trace.label == context && Trace.action == action && Trace.tokens == tokens, "input identities");
        check(Trace.first == (stage >= 3 ? mode == 8 ? null : String.class : Integer.class), "first read");
        check(Trace.length == (stage >= 3 ? tokens.length : -1), "array length");
        check(Trace.stamp == (mode == 13 ? Long.MIN_VALUE + 19 : stamp), "wide value");
        check(Trace.value == value, "callback mutation");
        if (tokens != null && tokens.length != 0)
            check(tokens[0] == (mode == 12 ? Double.class : mode == 8 ? null : String.class), "array mutation");
        check(failure == null ? thrown == null : thrown != null && thrown.getClass() == failure, "failure kind");
        if (mode == 4) check(thrown == callbackFailure, "exception identity");
        if (mode == 5) check(thrown == callbackError, "error identity");
        check(result == (failure == null && route != 2 ? value : ignored), "actual result and late cast");
    }

    public static void main(String[] args) throws Exception {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        for (int attempt = 0; attempt < 2; attempt++) attempt(route, mode, attempt);
        System.out.println("attempts=2,static-reifiable-results=matched");
    }
}
