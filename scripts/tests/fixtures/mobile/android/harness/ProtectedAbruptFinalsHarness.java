import fixture.*;
public final class ProtectedAbruptFinalsHarness {
    static final class FutureCore extends Core {
        FutureCore(boolean allowed, Object seed) { super(allowed, seed); }
    }
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned protected abrupt finals invariant");
    }
    public static void main(String[] args) {
        final int route = Integer.parseInt(args[0]);
        final int mode = Integer.parseInt(args[1]);
        final boolean allowed = route < 2;
        for (int attempt = 0; attempt < 2; ++attempt) {
            Core.latest = null; Trace.events = 0;
            final Object seed = mode == 15 ? null : new Object();
            final Object initial = mode == 8 ? null : new Object(), alternate = new Object();
            final Object marker = mode == 14 ? null : new Object();
            final Checked checked = new Checked();
            final RuntimeException callbackFailure = new RuntimeException();
            final RuntimeException producerFailure = new RuntimeException();
            final RuntimeException afterFailure = new RuntimeException();
            final RuntimeException guardFailure = new RuntimeException();
            final Error callbackError = new AssertionError("callback sentinel");
            final Error producerError = new AssertionError("producer sentinel");
            final int[] calls = {0, 0, 0};
            Trace.marker = Trace.saved = Trace.afterMarker = Trace.rejected = null;
            Trace.replacement = initial;
            Trace.checked = mode == 4 ? checked : null;
            Trace.failure = mode == 5 ? producerFailure : null;
            Trace.error = mode == 6 ? producerError : null;
            Trace.afterFailure = mode == 11 ? afterFailure : null;
            Trace.guardFailure = mode == 13 ? null : guardFailure;
            Trace.probe = mode == 1 ? null : new Probe() {
                public Object observe(Core self, int phase, Object value) {
                    require(self == Core.latest && ++calls[phase] == 1);
                    require((self instanceof FutureCore) == (route == 1 || route == 3));
                    if (phase == 1) {
                        require(allowed && value == seed && self.value == null && self.stage == 1 && Trace.events == 1);
                        require(Trace.marker == null && Trace.saved == null && calls[2] == 0);
                        if (mode == 2) throw callbackFailure;
                        if (mode == 3) throw callbackError;
                        if (mode == 7) Trace.replacement = alternate;
                        if (mode == 12) Trace.checked = checked;
                    } else if (phase == 2) {
                        Object actual = mode == 7 ? alternate : initial;
                        require(allowed && value == actual && self.value == actual && self.stage == 2 && Trace.events == 38);
                        require(Trace.saved == actual && Trace.marker == marker && Trace.afterMarker == null && calls[1] == 1);
                        if (mode == 9) throw callbackFailure;
                        if (mode == 10) throw callbackError;
                    } else {
                        require(!allowed && phase == 0 && value == null && self.value == null && self.stage == 1 && Trace.events == 6);
                        if (mode == 2) throw callbackFailure;
                        if (mode == 3) throw callbackError;
                    }
                    return marker;
                }
            };
            Core result = null; Throwable failure = null;
            try {
                result = route == 0 || route == 2 ? new Core(allowed, seed) : new FutureCore(allowed, seed);
            } catch (Throwable caught) { failure = caught; }
            boolean beforeWrite = !allowed || mode == 1 || mode == 2 || mode == 3 || mode == 4 || mode == 5 || mode == 6 || mode == 12;
            boolean afterFailed = mode == 9 || mode == 10 || mode == 11;
            boolean success = allowed && !beforeWrite && !afterFailed;
            int events = !allowed ? 6 : beforeWrite ? 1 : afterFailed ? 38 : 194;
            int stage = beforeWrite ? 1 : success ? 3 : 2;
            Core partial = Core.latest;
            Object actual = mode == 7 ? alternate : initial;
            require(partial != null && partial.stage == stage && partial.value == (beforeWrite ? null : actual));
            require(Trace.events == events && result == (success ? partial : null));
            require(calls[0] == (!allowed && mode != 1 ? 1 : 0));
            require(calls[1] == (allowed && mode != 1 ? 1 : 0));
            require(calls[2] == (allowed && !beforeWrite ? 1 : 0));
            require(Trace.saved == (beforeWrite ? null : actual));
            require(Trace.marker == (!allowed || mode == 1 || mode == 2 || mode == 3 ? null : marker));
            require(Trace.afterMarker == (beforeWrite || mode == 9 || mode == 10 ? null : marker));
            require(Trace.rejected == (allowed || mode == 1 || mode == 2 || mode == 3 ? null : marker));
            if (!allowed) {
                if (mode == 1 || mode == 13) require(failure instanceof NullPointerException);
                else if (mode == 2) require(failure == callbackFailure);
                else if (mode == 3) require(failure == callbackError);
                else require(failure == guardFailure);
            } else if (mode == 1) require(failure instanceof NullPointerException);
            else if (mode == 2 || mode == 9) require(failure == callbackFailure);
            else if (mode == 3 || mode == 10) require(failure == callbackError);
            else if (mode == 4 || mode == 12) require(failure instanceof IllegalStateException && failure.getCause() == checked);
            else if (mode == 5) require(failure == producerFailure);
            else if (mode == 6) require(failure == producerError);
            else if (mode == 11) require(failure == afterFailure);
            else require(failure == null);
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",events=" + events);
        }
    }
}
