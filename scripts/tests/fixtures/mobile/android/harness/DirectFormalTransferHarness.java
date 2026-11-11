import fixture.Core;
import fixture.Trace;
import java.io.IOException;

public final class DirectFormalTransferHarness {
    private static void check(boolean value, String reason) {
        if (!value) throw new AssertionError("owned direct formal transfer mismatch: " + reason);
    }
    private static void run(int route, int mode) {
        final boolean checked = route == 4 || route == 5;
        final boolean narrow = route == 1 || route == 5;
        final Object input = route == 3 || route == 6
                ? (mode % 2 == 0 ? String.class : null)
                : mode == 3 ? null : mode == 5 ? new String[]{"owned-array"} : new String("owned-seed");
        final Object initial = mode == 1 ? Integer.valueOf(71) : mode == 2 ? null
                : mode == 4 ? input : mode == 5 ? new Object[]{"owned-result"} : new String("owned-result");
        final Object after = mode == 10 ? new String("owned-after") : mode == 11 ? Integer.valueOf(93) : initial;
        final Object initialMarker = new Object(), changedMarker = new Object(), changedSeed = new Object();
        final RuntimeException runtime = new IllegalStateException("owned-runtime");
        final Error error = new LinkageError("owned-error");
        final IOException io = new IOException("owned-checked");
        Trace.seed = Trace.callbackSeed = null;
        Trace.value = initial;
        Trace.marker = initialMarker;
        Trace.constructing = null;
        Trace.failure = mode == 13 ? io : null;
        Trace.calls = Trace.callbacks = Trace.stage = Trace.entry = 0;
        Trace.action = mode == 9 ? null : () -> {
            check(Trace.calls == 1 && Trace.stage == 1 && Trace.entry == (checked ? 2 : 1), "callback boundary");
            if (route == 6) check(Trace.constructing != null && Trace.constructing.token == null, "constructor partial store");
            Trace.callbacks++;
            Trace.callbackSeed = Trace.seed;
            if (mode == 5 && input instanceof Object[]) ((Object[])input)[0] = "owned-mutated";
            if (mode == 6) throw runtime;
            if (mode == 7) throw error;
            if (mode == 8) throw (RuntimeException)null;
            if (mode == 10 || mode == 11) Trace.value = after;
            if (mode == 12) Trace.marker = changedMarker;
            if (mode == 14) Trace.failure = io;
            if (mode == 15) Trace.seed = changedSeed;
        };
        Object value = null;
        Throwable failure = null;
        try {
            switch (route) {
            case 0: value = Core.raw(input); break;
            case 1: value = Core.narrow(input); break;
            case 2: value = Core.ignored(input); break;
            case 3: value = Core.token((Class<?>)input); break;
            case 4: value = Core.checked(input); break;
            case 5: value = Core.checkedNarrow(input); break;
            case 6: value = new Core((Class<?>)input); break;
            default: throw new AssertionError("owned route");
            }
        } catch (Throwable caught) { failure = caught; }
        boolean actionFailure = mode >= 6 && mode <= 9;
        boolean ioFailure = checked && (mode == 13 || mode == 14);
        boolean castFailure = narrow && after != null && !(after instanceof String);
        if (mode == 6) check(failure == runtime, "exact runtime failure");
        else if (mode == 7) check(failure == error, "exact error");
        else if (mode == 8 || mode == 9) check(failure instanceof NullPointerException, "null action failure");
        else if (ioFailure) check(failure == io, "exact checked failure");
        else if (castFailure) check(failure instanceof ClassCastException, "original result cast");
        else check(failure == null, "unexpected failure");
        check(Trace.calls == 1 && Trace.entry == (checked ? 2 : 1), "single original call");
        check(Trace.stage == (actionFailure ? 1 : 2), "provider stage");
        check(Trace.callbacks == (mode == 9 ? 0 : 1), "callback count");
        check(Trace.callbackSeed == (mode == 9 ? null : input), "original seed identity");
        check(Trace.seed == (mode == 15 ? changedSeed : input), "seed field effects");
        if (mode == 5 && input instanceof Object[]) check(((Object[])input)[0].equals("owned-mutated"), "array identity");
        if (route == 6) {
            check(Trace.constructing != null && Trace.constructing.token == (actionFailure ? null : input), "constructor original token");
            if (!actionFailure) check(value == Trace.constructing, "constructor identity");
        } else if (!actionFailure && !ioFailure && !castFailure) {
            Object expected = route == 2 ? (mode == 12 ? changedMarker : initialMarker) : route == 3 ? input : after;
            check(value == expected, "actual result");
        }
    }
    public static void main(String[] args) {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        for (int attempt = 0; attempt < 2; ++attempt) run(route, mode);
        System.out.println("attempts=2,direct-formal-transfer=matched");
    }
}
