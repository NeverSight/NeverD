import fixture.*;
public final class ClosedCovariantDelegationHarness {
    static final class FutureCarrier extends Carrier<Payload> { }
    static final class FutureCore extends Core {
        FutureCore(Carrier<? extends Bound> value) { super(value); }
        FutureCore(String value) { super(value); }
    }
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned closed covariant delegation invariant");
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    static void poison(Carrier<?> value) { ((Carrier) value).value = new Bound(); }
    public static void main(String[] args) {
        final int route = Integer.parseInt(args[0]);
        final int mode = Integer.parseInt(args[1]);
        final boolean prefix = route < 2;
        for (int attempt = 0; attempt < 2; ++attempt) {
            final Carrier<Payload> source = mode == 14 ? new FutureCarrier() : new Carrier<Payload>();
            source.value = new Payload();
            if (mode == 6) poison(source);
            final String key = mode == 12 ? null : new String("key" + attempt);
            final String alternate = new String("alternate");
            final Object marker = mode == 15 ? null : new Object();
            final RuntimeException prefixCallback = new RuntimeException();
            final RuntimeException prefixFailure = new RuntimeException();
            final RuntimeException bodyCallback = new RuntimeException();
            final RuntimeException bodyFailure = new RuntimeException();
            final Error prefixError = new AssertionError("prefix sentinel");
            final Error bodyError = new AssertionError("body sentinel");
            final int[] calls = {0, 0};
            Core.latest = null;
            Names.events = 0;
            Names.savedClass = null; Names.savedResult = null; Names.prefixMarker = null;
            Names.replacement = key;
            Names.prefixFailure = mode == 5 || mode == 14 ? prefixFailure : null;
            Names.bodyFailure = mode == 11 ? bodyFailure : null;
            Names.prefixProbe = mode == 2 ? null : new Probe() {
                public Object observe(Object owner, Object value) {
                    require(++calls[0] == 1 && owner == source.getClass() && value == key);
                    require(Names.events == 1 && Names.savedClass == owner && Core.latest == null);
                    require(Names.savedResult == null && Names.prefixMarker == null);
                    if (mode == 3) throw prefixCallback;
                    if (mode == 4) throw prefixError;
                    if (mode == 6) Names.replacement = alternate;
                    if (mode == 7) Names.prefixFailure = prefixFailure;
                    if (mode == 14) Names.prefixFailure = null;
                    return marker;
                }
            };
            Names.bodyProbe = mode == 8 ? null : new Probe() {
                public Object observe(Object owner, Object value) {
                    Core self = (Core) owner;
                    String actual = prefix && mode == 6 ? alternate : key;
                    require(++calls[1] == 1 && owner == Core.latest && value == actual);
                    require(self.key == actual && self.events == 1 && self.marker == null && self.finished == 0);
                    require(Names.events == (prefix ? 7 : 0));
                    require(Names.savedResult == (prefix ? actual : null));
                    require((self instanceof FutureCore) == (route == 1 || route == 3));
                    if (mode == 9) throw bodyCallback;
                    if (mode == 10) throw bodyError;
                    if (mode == 13) Names.bodyFailure = bodyFailure;
                    return marker;
                }
            };
            Core result = null; Throwable failure = null;
            try {
                Carrier<? extends Bound> supplied = mode == 1 ? null : source;
                if (route == 0) result = new Core(supplied);
                else if (route == 1) result = new FutureCore(supplied);
                else if (route == 2) result = new Core(key);
                else result = new FutureCore(key);
            } catch (Throwable caught) { failure = caught; }
            boolean prefixFailed = prefix && (mode == 1 || mode == 2 || mode == 3 || mode == 4 || mode == 5 || mode == 7);
            boolean bodyFailed = mode == 8 || mode == 9 || mode == 10 || mode == 11 || mode == 13;
            boolean success = !prefixFailed && !bodyFailed;
            int expectedEvents = prefixFailed ? 0 : mode == 8 || mode == 9 || mode == 10 ? 1 : mode == 11 || mode == 13 ? 7 : 38;
            require(Names.events == (!prefix || mode == 1 ? 0 : prefixFailed ? 1 : 7));
            require(calls[0] == (!prefix || mode == 1 || mode == 2 ? 0 : 1));
            require(Names.savedClass == (!prefix || mode == 1 ? null : source.getClass()));
            require(Names.prefixMarker == (!prefix || mode == 1 || mode == 2 || mode == 3 || mode == 4 ? null : marker));
            String actual = prefix && mode == 6 ? alternate : key;
            require(Names.savedResult == (prefix && !prefixFailed ? actual : null));
            require(calls[1] == (prefixFailed || mode == 8 ? 0 : 1));
            if (prefixFailed) {
                require(Core.latest == null && result == null);
                if (mode == 1 || mode == 2) require(failure instanceof NullPointerException);
                else if (mode == 3) require(failure == prefixCallback);
                else if (mode == 4) require(failure == prefixError);
                else require(failure == prefixFailure);
            } else {
                Core partial = Core.latest;
                require(partial != null && partial.key == actual && partial.events == expectedEvents);
                require(partial.marker == (mode == 8 || mode == 9 || mode == 10 ? null : marker));
                require(partial.finished == (success ? prefix ? 9 : 7 : 0));
                require(result == (success ? partial : null));
                if (mode == 8) require(failure instanceof NullPointerException);
                else if (mode == 9) require(failure == bodyCallback);
                else if (mode == 10) require(failure == bodyError);
                else if (mode == 11 || mode == 13) require(failure == bodyFailure);
                else require(failure == null);
                if (success && mode == 6) {
                    boolean bad = false;
                    try { Payload ignored = source.value; }
                    catch (ClassCastException caught) { bad = true; }
                    require(bad && partial.events == 38);
                }
            }
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",events=" + expectedEvents);
        }
    }
}
