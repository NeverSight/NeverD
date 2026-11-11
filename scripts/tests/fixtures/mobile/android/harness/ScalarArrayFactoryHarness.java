import fixture.Core;
import fixture.Trace;
import java.util.HashSet;
import java.util.LinkedHashSet;

public final class ScalarArrayFactoryHarness {
    private static void check(boolean value, String reason) {
        if (!value) throw new AssertionError("owned scalar array factory mismatch: " + reason);
    }
    private static void run(int route, int mode) {
        final boolean flag = mode % 2 == 0;
        final long stamp = route == 4 ? 0x123456789ABCDEFL + mode : 0L;
        final Object[] values = mode == 1 || mode == 17 ? null
                : mode == 2 || mode == 18 ? (route == 1 ? new String[0] : new Object[0])
                : route == 1 || mode == 4 || mode == 12 ? new String[]{new String("owned-element")}
                : new Object[]{new String("owned-element")};
        final Object initialElement = values == null || values.length == 0 ? null : values[0];
        final Object saved = mode == 4 ? Integer.valueOf(53) : new String("owned-store");
        final HashSet<Object> result = mode == 5 ? new LinkedHashSet<>() : new HashSet<>();
        result.add(new Object());
        final HashSet<Object> changedResult = new HashSet<>();
        changedResult.add(new Object());
        final Object expectedResult = mode == 3 ? null : mode == 10 ? changedResult : result;
        final Object marker = new Object(), changedMarker = new Object();
        final Object[] changedValues = new Object[]{"owned-replacement-array"};
        final RuntimeException runtime = new IllegalStateException("owned-runtime");
        final Error error = new LinkageError("owned-error");
        Trace.element = Trace.saved = null;
        Trace.values = null;
        Trace.value = mode == 3 ? null : mode == 11 ? Integer.valueOf(77) : result;
        Trace.store = saved;
        Trace.marker = marker;
        Trace.flag = !flag;
        Trace.afterFlag = !flag;
        Trace.stamp = Trace.afterStamp = Long.MIN_VALUE;
        Trace.inspect = mode != 17 && mode != 18;
        Trace.write = mode != 17 && mode != 18;
        Trace.index = mode == 13 ? 5 : 0;
        Trace.calls = Trace.forwards = Trace.callbacks = Trace.stage = Trace.entry = 0;
        Trace.action = mode == 9 ? null : () -> {
            check(Trace.stage == (mode == 17 || mode == 18 ? 1 : 2), "callback stage");
            check(Trace.flag == flag && Trace.stamp == stamp && Trace.values == values, "original callback arguments");
            Trace.callbacks++;
            if (mode == 6) throw runtime;
            if (mode == 7) throw error;
            if (mode == 8) throw (RuntimeException)null;
            if (mode == 10) Trace.value = changedResult;
            if (mode == 12) Trace.store = Integer.valueOf(61);
            if (mode == 14) Trace.values = changedValues;
            if (mode == 15) Trace.flag = !flag;
            if (mode == 16) Trace.stamp = -71L;
            if (mode == 19) Trace.marker = changedMarker;
        };
        Object value = null;
        Throwable failure = null;
        try {
            switch (route) {
            case 0: value = Core.raw(flag, values); break;
            case 1: value = Core.typed(flag, (String[])values); break;
            case 2: value = Core.ignored(flag, values); break;
            case 3: value = Core.fixed(flag, values); break;
            case 4: value = Core.wide(flag, stamp, values); break;
            default: throw new AssertionError("owned route");
            }
        } catch (Throwable caught) { failure = caught; }
        if (mode == 1 || mode == 8 || mode == 9) check(failure instanceof NullPointerException, "original null failure");
        else if (mode == 2 || mode == 13) check(failure instanceof ArrayIndexOutOfBoundsException, "original bounds failure");
        else if (mode == 4) check(failure instanceof ArrayStoreException, "covariant array store");
        else if (mode == 6) check(failure == runtime, "exact runtime failure");
        else if (mode == 7) check(failure == error, "exact error");
        else if (mode == 11) check(failure instanceof ClassCastException, "provider result cast");
        else check(failure == null, "unexpected failure");
        int expectedStage = mode == 1 || mode == 2 ? 1 : mode >= 6 && mode <= 9 ? 2
                : mode == 4 || mode == 13 || mode == 17 || mode == 18 ? 3 : 4;
        check(Trace.calls == 1 && Trace.forwards == (route == 3 ? 1 : 0), "single call and forward");
        check(Trace.entry == (route == 4 ? 2 : 1) && Trace.stage == expectedStage, "entry and stage");
        check(Trace.saved == saved && Trace.values == (mode == 14 ? changedValues : values), "array and store snapshot");
        check(Trace.callbacks == (mode == 1 || mode == 2 || mode == 9 ? 0 : 1), "callback count");
        check(Trace.element == (mode == 1 || mode == 2 || mode == 17 || mode == 18 ? null : initialElement), "element snapshot");
        check(Trace.flag == (mode == 15 ? !flag : flag) && Trace.stamp == (mode == 16 ? -71L : stamp), "scalar field effects");
        check(Trace.afterFlag == (expectedStage >= 3 ? flag : !flag), "original boolean snapshot");
        check(Trace.afterStamp == (expectedStage >= 3 ? stamp : Long.MIN_VALUE), "original wide snapshot");
        if (expectedStage == 4 && values != null && values.length != 0) check(values[0] == saved, "original array write");
        if (mode == 14) check(changedValues[0].equals("owned-replacement-array"), "array field is not parameter");
        if (failure == null) check(value == (route == 2 ? (mode == 19 ? changedMarker : marker) : expectedResult), "actual provider result");
    }
    public static void main(String[] args) {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        for (int attempt = 0; attempt < 2; ++attempt) run(route, mode);
        System.out.println("attempts=2,scalar-array-factory=matched");
    }
}
