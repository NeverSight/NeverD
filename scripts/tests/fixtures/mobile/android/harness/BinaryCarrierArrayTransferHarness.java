import fixture.Box;
import fixture.Core;
import fixture.Trace;

public final class BinaryCarrierArrayTransferHarness {
    private static void check(boolean value, String reason) {
        if (!value) throw new AssertionError("owned binary carrier array transfer mismatch: " + reason);
    }
    @SuppressWarnings("unchecked")
    private static void run(int route, int mode) {
        final Object oldMarker = new Object(), changedInputMarker = new Object();
        final Box input = mode == 1 || mode == 17 ? null : new Box(oldMarker);
        final Object[] values = mode == 2 || mode == 17 ? null
                : mode == 3 || mode == 18 ? (route == 1 ? new String[0] : new Object[0])
                : route == 1 || mode == 7 || mode == 12 ? new String[]{new String("owned-element")}
                : new Object[]{new String("owned-element")};
        final Object initialElement = values == null || values.length == 0 ? null : values[0];
        final Object saved = mode == 7 ? Integer.valueOf(53) : new String("owned-store");
        final Box result = new Box(new Object()), changedResult = new Box(new Object());
        final Object expectedResult = mode == 4 ? null : mode == 5 ? input : mode == 15 ? changedResult : result;
        final Object marker = new Object(), changedMarker = new Object();
        final Object[] changedValues = new Object[]{"owned-replacement-array"};
        final RuntimeException runtime = new IllegalStateException("owned-runtime");
        final Error error = new LinkageError("owned-error");
        Trace.input = Trace.inputMarker = Trace.element = Trace.saved = null;
        Trace.values = null;
        Trace.value = mode == 4 ? null : mode == 5 ? input : mode == 16 ? Integer.valueOf(77) : result;
        Trace.store = saved;
        Trace.marker = marker;
        Trace.inspectInput = mode != 17;
        Trace.inspectArray = mode != 17 && mode != 18;
        Trace.write = mode != 17 && mode != 18;
        Trace.index = mode == 13 ? 5 : 0;
        Trace.calls = Trace.forwards = Trace.callbacks = Trace.stage = Trace.entry = 0;
        Trace.action = mode == 11 ? null : () -> {
            check(Trace.stage == (mode == 17 ? 1 : mode == 18 ? 2 : 3), "callback stage");
            check(Trace.input == input && Trace.values == values, "original callback references");
            Trace.callbacks++;
            if (mode == 6) input.marker = changedInputMarker;
            if (mode == 8) throw runtime;
            if (mode == 9) throw error;
            if (mode == 10) throw (RuntimeException)null;
            if (mode == 12) Trace.store = Integer.valueOf(61);
            if (mode == 14) Trace.values = changedValues;
            if (mode == 15) Trace.value = changedResult;
            if (mode == 19) Trace.marker = changedMarker;
        };
        Object value = null;
        Throwable failure = null;
        try {
            switch (route) {
            case 0: value = Core.raw(input, values); break;
            case 1: value = Core.typed((Box<String,Integer>)input, (String[])values); break;
            case 2: value = Core.ignored(input, values); break;
            case 3: value = Core.second(input, values); break;
            case 4: value = Core.fixed(input, values); break;
            default: throw new AssertionError("owned route");
            }
        } catch (Throwable caught) { failure = caught; }
        if (mode == 1 || mode == 2 || mode == 10 || mode == 11) check(failure instanceof NullPointerException, "original null failure");
        else if (mode == 3 || mode == 13) check(failure instanceof ArrayIndexOutOfBoundsException, "original bounds failure");
        else if (mode == 7) check(failure instanceof ArrayStoreException, "covariant array store");
        else if (mode == 8) check(failure == runtime, "exact runtime failure");
        else if (mode == 9) check(failure == error, "exact error");
        else if (mode == 16) check(failure instanceof ClassCastException, "provider result cast");
        else check(failure == null, "unexpected failure");
        int expectedStage = mode == 1 ? 1 : mode == 2 || mode == 3 ? 2
                : mode >= 8 && mode <= 11 ? 3 : mode == 7 || mode == 13 || mode == 17 || mode == 18 ? 4 : 5;
        check(Trace.calls == 1 && Trace.forwards == (route == 4 ? 1 : 0), "single call and forward");
        check(Trace.entry == (route == 3 ? 2 : 1) && Trace.stage == expectedStage, "entry and stage");
        check(Trace.saved == saved && Trace.input == input, "input and store snapshot");
        check(Trace.values == (mode == 14 ? changedValues : values), "array field effects");
        check(Trace.callbacks == (mode == 1 || mode == 2 || mode == 3 || mode == 11 ? 0 : 1), "callback count");
        check(Trace.inputMarker == (mode == 1 || mode == 17 ? null : oldMarker), "input field snapshot");
        check(Trace.element == (mode == 1 || mode == 2 || mode == 3 || mode == 17 || mode == 18 ? null : initialElement), "element snapshot");
        if (input != null) check(input.marker == (mode == 6 ? changedInputMarker : oldMarker), "carrier mutation");
        if (expectedStage == 5 && values != null && values.length != 0) check(values[0] == saved, "original array write");
        if (mode == 14) check(changedValues[0].equals("owned-replacement-array"), "array field is not parameter");
        if (failure == null) check(value == (route == 2 ? (mode == 19 ? changedMarker : marker) : expectedResult), "actual provider result");
    }
    public static void main(String[] args) {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        for (int attempt = 0; attempt < 2; ++attempt) run(route, mode);
        System.out.println("attempts=2,binary-carrier-array-transfer=matched");
    }
}
