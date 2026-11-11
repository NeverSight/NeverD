import fixture.*;
public final class TokenSuperResultHarness {
    static final class FutureCore extends Core {
        FutureCore(Factory source, int id) { super(source, id); }
    }
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned token super result invariant");
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    static void put(Carrier<?> source, Bound value) { ((Carrier) source).value = value; }
    public static void main(String[] args) {
        final int route = Integer.parseInt(args[0]);
        final int mode = Integer.parseInt(args[1]);
        final int id = new int[]{Integer.MIN_VALUE, Integer.MAX_VALUE, -1, 0}[route];
        for (int attempt = 0; attempt < 2; ++attempt) {
            final Carrier<Payload> source = mode == 14 ? new Carrier<Payload>() : new Leaf();
            final Factory factory = new Factory();
            Factory.events = 0;
            factory.replacement = mode == 15 ? null : source;
            factory.failure = mode == 12 ? new RuntimeException() : null;
            final Payload initial = new Payload(), alternate = new Payload();
            final Bound poisoned = new Bound();
            final Object marker = mode == 10 ? null : new Object();
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
                    require(Guard.calls == 2 && Guard.last == source && Factory.events == 1 && factory.savedToken == Leaf.class && self.source == source);
                    require(self.id == id && self.stage == 1 && self.value == null && self.marker == null);
                    require(((Core) self).finished == 0);
                    if (mode == 6) throw callbackFailure;
                    if (mode == 7) throw callbackError;
                    if (mode == 8) source.value = alternate;
                    if (mode == 9) source.failure = targetFailure;
                    if (mode == 11) put(source, poisoned);
                    return marker;
                }
            };
            if (mode == 14) put(source, poisoned);
            Core result = null;
            Throwable failure = null;
            try {
                Factory argument = mode == 1 ? null : factory;
                result = route % 2 == 0 ? new Core(argument, id) : new FutureCore(argument, id);
            } catch (Throwable caught) { failure = caught; }
            boolean early = mode == 1 || mode == 3 || mode == 4 || mode == 12 || mode == 13 || mode == 15;
            boolean callbackFailed = mode == 2 || mode == 6 || mode == 7;
            boolean targetFailed = mode == 5 || mode == 9;
            boolean success = !early && !callbackFailed && !targetFailed;
            int stage = early ? 0 : callbackFailed ? 1 : targetFailed ? 12 : 123;
            boolean beforeFactory = mode == 1 || mode == 3 || mode == 4 || mode == 13;
            require(Guard.calls == (beforeFactory || mode == 12 ? 1 : 2));
            require(Guard.last == (beforeFactory ? mode == 1 ? null : factory : mode == 12 ? factory : mode == 15 ? null : source));
            require(Factory.events == (beforeFactory ? 0 : 1));
            require(factory.savedToken == (beforeFactory ? null : Leaf.class));
            require(calls[0] == (early || mode == 2 ? 0 : 1));
            if (mode == 1 || mode == 2 || mode == 15) require(failure instanceof NullPointerException);
            else if (mode == 12) require(failure == factory.failure);
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
                        : mode == 10 ? null : initial;
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
