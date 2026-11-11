import fixture.Config;
import fixture.Core;
import fixture.Trace;
import java.util.Map;
import java.util.HashMap;

public final class ReifiableSeedContextHarness {
    private static void check(boolean value, String reason) {
        if (!value) throw new AssertionError("owned reifiable seed context mismatch: " + reason);
    }
    private static void run(int route, int mode) {
        final boolean flag = route == 3 ? false : mode % 2 == 0;
        final Map<Object,Object> map = mode == 1 || mode == 17 ? null : new HashMap<>();
        if (map != null && mode != 3) map.put("owned-original", new Object());
        final int beforeSize = map == null ? -1 : map.size();
        final Object configMarker = new Object(), changedConfigMarker = new Object();
        final Config config = mode == 2 ? null : new Config(configMarker);
        final String seed = mode == 4 ? null : new String("owned-seed");
        final boolean arrayResult = route >= 5;
        final Object initial = arrayResult
                ? mode == 5 ? null : mode == 7 ? new Object[]{"owned-element"} : new String[]{"owned-result"}
                : mode == 5 ? null : mode == 6 ? seed : mode == 7 ? Integer.valueOf(31) : new String("owned-result");
        final Object after = arrayResult
                ? mode == 12 ? new String[]{"owned-after"} : mode == 13 ? new Object[]{"owned-after"} : mode == 18 ? Integer.valueOf(71) : initial
                : mode == 12 ? new String("owned-after") : mode == 13 ? Integer.valueOf(51) : initial;
        final Object marker = new Object(), changedMarker = new Object(), changedSeed = new Object();
        final RuntimeException runtime = new IllegalStateException("owned-runtime");
        final Error error = new LinkageError("owned-error");
        Trace.map = Trace.seed = Trace.config = Trace.configMarker = null;
        Trace.value = initial;
        Trace.marker = marker;
        Trace.flag = Trace.afterFlag = !flag;
        Trace.inspect = mode != 17;
        Trace.calls = Trace.relays = Trace.callbacks = Trace.stage = 0;
        Trace.size = -1;
        Trace.action = mode == 11 ? null : () -> {
            check(Trace.stage == (mode == 17 ? 2 : 3), "callback stage");
            check(Trace.map == map && Trace.seed == seed && Trace.config == config && Trace.flag == flag, "original argument references");
            Trace.callbacks++;
            if (mode == 8) throw runtime;
            if (mode == 9) throw error;
            if (mode == 10) throw (RuntimeException)null;
            if (mode == 12 || mode == 13) Trace.value = after;
            if (mode == 14) map.put("owned-after", new Object());
            if (mode == 15) config.marker = changedConfigMarker;
            if (mode == 16) Trace.flag = !flag;
            if (mode == 18) { Trace.seed = changedSeed; if (arrayResult) Trace.value = after; }
            if (mode == 19) Trace.marker = changedMarker;
        };
        Object value = null;
        Throwable failure = null;
        try {
            switch (route) {
            case 0: value = Core.raw(map, seed, flag, config); break;
            case 1: value = Core.narrow(map, seed, flag, config); break;
            case 2: value = Core.ignored(map, seed, flag, config); break;
            case 3: value = Core.relay(map, seed, config); break;
            case 4: value = Core.typed(map, seed, flag, config); break;
            case 5: value = Core.array(map, seed, flag, config); break;
            case 6: value = Core.typedArray(map, seed, flag, config); break;
            default: throw new AssertionError("owned route");
            }
        } catch (Throwable caught) { failure = caught; }
        boolean castFailure = arrayResult
                ? after != null && (!(after instanceof Object[]) || (route == 6 && !(after instanceof String[])))
                : (route == 1 || route == 4) && after != null && !(after instanceof String);
        if (mode == 1 || mode == 2 || mode == 10 || mode == 11) check(failure instanceof NullPointerException, "original null failure");
        else if (mode == 8) check(failure == runtime, "exact runtime failure");
        else if (mode == 9) check(failure == error, "exact error");
        else if (castFailure) check(failure instanceof ClassCastException, "original result cast");
        else check(failure == null, "unexpected failure");
        int expectedStage = mode == 2 ? 1 : mode == 1 ? 2 : mode >= 8 && mode <= 11 ? 3 : 4;
        check(Trace.calls == 1 && Trace.relays == (route == 3 ? 1 : 0) && Trace.stage == expectedStage, "single call, relay and stage");
        check(Trace.map == map && Trace.config == config && Trace.seed == (mode == 18 ? changedSeed : seed), "original references and field effects");
        check(Trace.configMarker == (mode == 2 ? null : configMarker), "configuration snapshot");
        check(Trace.size == (mode == 1 || mode == 2 || mode == 17 ? -1 : beforeSize), "map size snapshot");
        check(Trace.callbacks == (mode == 1 || mode == 2 || mode == 11 ? 0 : 1), "callback count");
        check(Trace.flag == (mode == 16 ? !flag : flag) && Trace.afterFlag == (expectedStage == 4 ? flag : !flag), "boolean snapshot");
        if (map != null) check(map.size() == beforeSize + (mode == 14 ? 1 : 0), "original map mutation");
        if (config != null) check(config.marker == (mode == 15 ? changedConfigMarker : configMarker), "original configuration mutation");
        if (failure == null) check(value == (route == 2 ? (mode == 19 ? changedMarker : marker) : after), "actual provider result");
    }
    public static void main(String[] args) {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        for (int attempt = 0; attempt < 2; ++attempt) run(route, mode);
        System.out.println("attempts=2,reifiable-seed-context=matched");
    }
}
