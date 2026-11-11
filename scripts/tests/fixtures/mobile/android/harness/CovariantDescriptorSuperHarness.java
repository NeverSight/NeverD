import fixture.*;
public final class CovariantDescriptorSuperHarness {
    static final class FutureCore extends Core {
        FutureCore(Leaf source, int id) { super(source, id); }
    }
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned covariant descriptor super invariant");
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    static void put(Leaf source, Bound value) { ((Carrier) source).value = value; }
    public static void main(String[] args) {
        final int route = Integer.parseInt(args[0]);
        final int mode = Integer.parseInt(args[1]);
        final int id = new int[]{Integer.MIN_VALUE, Integer.MAX_VALUE, -1, 0}[route];
        for (int attempt = 0; attempt < 2; ++attempt) {
            final Leaf source = new Leaf();
            final Payload initial = new Payload(), alternate = new Payload();
            final Bound poisoned = new Bound();
            final Object marker = mode == 15 ? null : new Object();
            final RuntimeException guardFailure = new RuntimeException();
            final RuntimeException callbackFailure = new RuntimeException();
            final RuntimeException targetFailure = new RuntimeException();
            final Error guardError = new AssertionError("guard sentinel");
            final Error callbackError = new AssertionError("callback sentinel");
            final int[] calls = {0};
            Guard.calls = 0; Guard.last = null;
            Guard.failure = mode == 3 || mode == 13 ? guardFailure : null;
            Guard.error = mode == 4 ? guardError : null;
            Builder.latest = null;
            source.value = mode == 10 ? null : initial;
            source.failure = mode == 5 || mode == 13 ? targetFailure : null;
            source.probe = mode == 2 ? null : new Probe() {
                public Object observe(Object object, Object actualSource) {
                    Builder<?> self = (Builder<?>) object;
                    require(++calls[0] == 1 && actualSource == source && self == Builder.latest);
                    require(Guard.calls == 1 && Guard.last == source && self.source == source);
                    require(self.id == id && self.stage == 1 && self.value == null && self.marker == null);
                    require(((Core) self).finished == 0);
                    if (mode == 6) throw callbackFailure;
                    if (mode == 7) throw callbackError;
                    if (mode == 8) source.value = alternate;
                    if (mode == 9) source.failure = targetFailure;
                    if (mode == 11) put(source, poisoned);
                    if (mode == 12) source.value = null;
                    return marker;
                }
            };
            if (mode == 14) put(source, poisoned);
            Core result = null;
            Throwable failure = null;
            try {
                Leaf argument = mode == 1 ? null : source;
                result = route % 2 == 0 ? new Core(argument, id) : new FutureCore(argument, id);
            } catch (Throwable caught) { failure = caught; }
            boolean early = mode == 1 || mode == 3 || mode == 4 || mode == 13;
            boolean callbackFailed = mode == 2 || mode == 6 || mode == 7;
            boolean targetFailed = mode == 5 || mode == 9;
            boolean success = !early && !callbackFailed && !targetFailed;
            int stage = early ? 0 : callbackFailed ? 1 : targetFailed ? 12 : 123;
            require(Guard.calls == 1 && Guard.last == (mode == 1 ? null : source));
            require(calls[0] == (early || mode == 2 ? 0 : 1));
            if (mode == 1 || mode == 2) require(failure instanceof NullPointerException);
            else if (mode == 3 || mode == 13) require(failure == guardFailure);
            else if (mode == 4) require(failure == guardError);
            else if (mode == 6) require(failure == callbackFailure);
            else if (mode == 7) require(failure == callbackError);
            else if (targetFailed) require(failure == targetFailure);
            else require(failure == null);
            if (early) require(Builder.latest == null && result == null);
            else {
                Builder<?> partial = Builder.latest;
                require(partial instanceof Core && partial.source == source && partial.id == id);
                require(partial.stage == stage && ((Core) partial).finished == (success ? 9 : 0));
                require(partial.marker == (callbackFailed ? null : marker));
                Bound expected = mode == 8 ? alternate : mode == 11 || mode == 14 ? poisoned
                        : mode == 10 || mode == 12 ? null : initial;
                require(partial.value == (callbackFailed ? null : expected));
                require(result == (success ? partial : null));
                if (success) {
                    if (mode == 11 || mode == 14) {
                        boolean bad = false;
                        try { Payload ignored = result.value; }
                        catch (ClassCastException caught) { bad = true; }
                        require(bad && partial.stage == 123);
                    } else require(result.value == expected);
                }
            }
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",stage=" + stage);
        }
    }
}
