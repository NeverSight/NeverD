import fixture.Action;
import fixture.Core;
import fixture.Trace;
import java.util.Objects;

public final class PrivateStaticReifiableResultsHarness {
    private static void check(boolean condition, String label) {
        if (!condition) throw new AssertionError("owned private static result mismatch: " + label);
    }

    private static void attempt(final int route, final int mode, final int attempt) {
        final Object initial = mode == 2 ? null : mode == 1 || mode == 13 && attempt == 0
                ? Integer.valueOf(1234) : new String("initial");
        final Object replacement = new String("replacement");
        final RuntimeException callbackFailure = new IllegalArgumentException("callback");
        final Error callbackError = new LinkageError("callback");
        final Class<?> token = mode == 6 ? null : mode == 14 ? Integer.TYPE : String.class;
        final Class<?> first = mode == 9 ? null : token;
        final Class<?>[] tokens = mode == 7 ? null : mode == 8 ? new Class<?>[0]
                : new Class<?>[]{first, Boolean.class};
        final Class<?>[] replacementArray = new Class<?>[]{Long.class};
        final long changedStamp = Long.MIN_VALUE + 19;
        final long stamp = mode == 15 ? (attempt == 0 ? Long.MIN_VALUE + 3 : Long.MAX_VALUE - 5)
                : (attempt == 0 ? 0x1234567887654321L : -0x1234567887654321L);
        final Action action = mode == 3 ? null : new Action() {
            public void run() {
                Trace.callbacks++;
                if (mode == 4) throw callbackFailure;
                if (mode == 5) throw callbackError;
                if (mode == 10) Trace.value = replacement;
                if (mode == 11) Trace.value = null;
                if (mode == 12) {
                    Trace.token = Long.class;
                    Trace.array = replacementArray;
                    Trace.stamp = changedStamp;
                }
                if (mode == 16) tokens[0] = Double.class;
                if (mode == 17) ((Object[]) tokens)[0] = new Object();
            }
        };
        Trace.value = initial;
        Trace.token = null;
        Trace.array = null;
        Trace.action = null;
        Trace.name = null;
        Trace.stamp = 0;
        Trace.calls = Trace.callbacks = Trace.stage = 0;
        final Object ignored = new Object();
        Object result = ignored;
        Throwable thrown = null;
        try {
            switch (route) {
            case 0: result = Core.raw(action, token, stamp); break;
            case 1: result = Core.narrow(action, token, stamp); break;
            case 2: Core.ignore(action, token, stamp); break;
            case 3: result = Core.relay(action, token, stamp); break;
            case 4: result = Core.rawArray(action, stamp, tokens); break;
            case 5: result = Core.narrowArray(action, stamp, tokens); break;
            case 6: Core.ignoreArray(action, stamp, tokens); break;
            case 7: result = Core.relayArray(action, stamp, tokens); break;
            default: throw new AssertionError("route");
            }
        } catch (Throwable failure) {
            thrown = failure;
        }
        final boolean arrayRoute = route >= 4;
        final boolean entered = !arrayRoute || tokens != null && tokens.length != 0;
        final Class<?> actualToken = arrayRoute ? first : token;
        final boolean named = entered && actualToken != null;
        final int callbacks = named && action != null ? 1 : 0;
        final boolean completed = callbacks == 1 && mode != 4 && mode != 5 && mode != 17;
        final int stage = !entered ? 0 : !named ? 1 : !completed ? 2 : 3;
        final Object value = callbacks == 1 && mode == 10 ? replacement
                : callbacks == 1 && mode == 11 ? null : initial;
        final Class<?> failure = !entered ? (tokens == null ? NullPointerException.class : ArrayIndexOutOfBoundsException.class)
                : !named || action == null ? NullPointerException.class
                : mode == 4 ? IllegalArgumentException.class : mode == 5 ? LinkageError.class
                : mode == 17 ? ArrayStoreException.class
                : (route == 1 || route == 5) && value != null && !(value instanceof String) ? ClassCastException.class : null;
        check(Trace.calls == (entered ? 1 : 0) && Trace.callbacks == callbacks && Trace.stage == stage, "ordered effects");
        check(Trace.action == (entered ? action : null), "callback identity");
        check(Trace.token == (callbacks == 1 && mode == 12 ? Long.class : entered ? actualToken : null), "token identity");
        check(Trace.array == (callbacks == 1 && mode == 12 ? replacementArray : arrayRoute ? tokens : null), "array identity");
        check(Trace.stamp == (callbacks == 1 && mode == 12 ? changedStamp : entered ? stamp : 0), "wide value");
        check(Objects.equals(Trace.name, named ? actualToken.getName() : null), "token observation");
        check(Trace.value == value, "callback mutation");
        if (tokens != null && tokens.length != 0)
            check(tokens[0] == (callbacks == 1 && mode == 16 ? Double.class : first), "original array alias");
        check(failure == null ? thrown == null : thrown != null && thrown.getClass() == failure, "failure kind");
        if (mode == 4) check(thrown == callbackFailure, "exception identity");
        if (mode == 5) check(thrown == callbackError, "error identity");
        check(result == (failure == null && route != 2 && route != 6 ? value : ignored), "actual result and late cast");
    }

    public static void main(String[] args) {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        for (int attempt = 0; attempt < 2; attempt++) attempt(route, mode, attempt);
        System.out.println("attempts=2,private-static-reifiable-results=matched");
    }
}
