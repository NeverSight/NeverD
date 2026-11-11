import fixture.Core;
import fixture.Trace;
import java.io.IOException;
import java.util.Objects;

public final class ThrowingTokenArraysHarness {
    private static void check(boolean condition, String label) {
        if (!condition) throw new AssertionError("owned throwing token arrays mismatch: " + label);
    }

    private static void attempt(final int route, final int mode, final int attempt) {
        final Object initial = mode == 2 ? null : mode == 1 || mode == 17 && attempt == 0
                ? Integer.valueOf(1234) : new String("initial");
        final Object replacement = new String("replacement");
        final Object first = mode == 5 ? null : new String("element");
        final Object changedElement = new String("changed element");
        final Object[] values = mode == 3 ? null : mode == 4 ? new Object[0]
                : mode == 14 ? new String[]{(String) first} : new Object[]{first, new Object()};
        final Class<?> token = mode == 10 ? null : Integer.class;
        final IOException checkedFailure = new IOException("checked");
        final RuntimeException callbackFailure = new IllegalStateException("callback");
        final Error callbackError = new LinkageError("callback");
        final Runnable action = mode == 6 ? null : new Runnable() {
            public void run() {
                Trace.callbacks++;
                if (mode == 7) throw callbackFailure;
                if (mode == 8) throw callbackError;
                if (mode == 11) Trace.value = replacement;
                if (mode == 12) Trace.value = null;
                if (mode == 13) values[0] = changedElement;
                if (mode == 14) values[0] = Integer.valueOf(42);
                if (mode == 15) Trace.failure = checkedFailure;
                if (mode == 16) Trace.failure = null;
            }
        };
        Trace.value = initial;
        Trace.first = null;
        Trace.token = null;
        Trace.array = null;
        Trace.action = action;
        Trace.failure = mode == 9 || mode == 16 ? checkedFailure : null;
        Trace.name = null;
        Trace.calls = Trace.callbacks = Trace.stage = 0;
        Trace.length = -1;
        final Object ignored = new Object();
        Object result = ignored;
        Throwable thrown = null;
        try {
            switch (route) {
            case 0: result = Core.raw(token, values); break;
            case 1: result = Core.narrow(token, values); break;
            case 2: Core.ignore(token, values); break;
            case 3: result = Core.relay(token, values); break;
            case 4: result = Core.fixed(token, values); break;
            case 5: result = Core.retained(token, values); break;
            default: throw new AssertionError("route");
            }
        } catch (Throwable failure) {
            thrown = failure;
        }
        final boolean entered = route != 5 || values != null && values.length != 0;
        final boolean named = entered && token != null;
        final boolean lengthRead = named && values != null;
        final boolean firstRead = lengthRead && values.length != 0;
        final int callbacks = firstRead && action != null ? 1 : 0;
        final boolean completed = callbacks == 1 && mode != 7 && mode != 8 && mode != 9 && mode != 14 && mode != 15;
        final int stage = !entered ? 0 : !named ? 1 : !firstRead ? 2 : !completed ? 3 : 4;
        final Object value = callbacks == 1 && mode == 11 ? replacement
                : callbacks == 1 && mode == 12 ? null : initial;
        final Class<?> failure = !entered ? (values == null ? NullPointerException.class : ArrayIndexOutOfBoundsException.class)
                : !named || values == null ? NullPointerException.class
                : values.length == 0 ? ArrayIndexOutOfBoundsException.class
                : action == null ? NullPointerException.class
                : mode == 7 ? IllegalStateException.class : mode == 8 ? LinkageError.class
                : mode == 9 || mode == 15 ? IOException.class : mode == 14 ? ArrayStoreException.class
                : route == 1 && value != null && !(value instanceof String) ? ClassCastException.class : null;
        check(Trace.calls == (entered ? 1 : 0) && Trace.callbacks == callbacks && Trace.stage == stage, "ordered effects");
        check(Trace.token == (entered ? token : null) && Trace.array == (entered ? values : null), "input identities");
        check(Objects.equals(Trace.name, named ? token.getName() : null), "token observation");
        check(Trace.length == (lengthRead ? values.length : -1), "array length");
        check(Trace.first == (firstRead ? first : null), "original element snapshot");
        check(Trace.value == value, "callback result mutation");
        check(Trace.failure == (mode == 9 || mode == 15 ? checkedFailure : null), "checked failure state");
        if (values != null && values.length != 0)
            check(values[0] == (callbacks == 1 && mode == 13 ? changedElement : first), "array alias and store");
        check(failure == null ? thrown == null : thrown != null && thrown.getClass() == failure, "failure kind");
        if (mode == 7) check(thrown == callbackFailure, "unchecked exception identity");
        if (mode == 8) check(thrown == callbackError, "error identity");
        if (mode == 9 || mode == 15) check(thrown == checkedFailure, "checked exception identity");
        check(result == (failure == null && route != 2 ? route == 5 ? first : value : ignored), "result and late cast");
    }

    public static void main(String[] args) {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        for (int attempt = 0; attempt < 2; attempt++) attempt(route, mode, attempt);
        System.out.println("attempts=2,throwing-token-arrays=matched");
    }
}
