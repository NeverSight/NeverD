import fixture.*;
public final class ConditionalNestedCopiesHarness {
    static void require(boolean ok, String detail) {
        if (!ok) throw new AssertionError("conditional nested copies contract: " + detail);
    }
    public static void main(String[] args) throws Throwable {
        int variant = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]);
        boolean choose = Integer.parseInt(args[2]) != 0;
        int mode = Integer.parseInt(args[3]);
        long first = mode == 7 ? Long.MIN_VALUE : mode == 8 ? 0x7ff8000000000042L
            : mode == 9 ? 0xfff8000000000017L : mode == 10 ? 0x8000000000000000L : 0x3ff0000000000001L;
        long second = mode == 7 ? Long.MAX_VALUE : mode == 8 ? 0x8000000000000000L
            : mode == 9 ? 0x7ff0000000000000L : mode == 10 ? 0L : 0xbff0000000000002L;
        Object tag = mode == 11 ? null : new Object();
        boolean outerStarted = false, outerFailed = false, innerStarted = false, innerFailed = false;
        for (int attempt = 0; attempt < 2; ++attempt) {
            Trace.mode = mode; Trace.effects = 0; Trace.inner = null; Trace.outer = null;
            Trace.failure = new IllegalStateException("owned failure"); Trace.fallbackValue = new Object();
            long expected = 0;
            String failure = "none";
            boolean sawInner = false, sawOuter = false;
            if (outerFailed) failure = "repeat-init";
            else {
                if (!outerStarted) {
                    outerStarted = true; expected = 1;
                    if (mode == 1) { outerFailed = true; failure = "first-init"; }
                }
                if (failure.equals("none")) {
                    expected = expected * 10 + 2;
                    if (mode == 2) failure = "body";
                }
                if (failure.equals("none") && choose) {
                    if (innerFailed) failure = "repeat-init";
                    else {
                        if (!innerStarted) {
                            innerStarted = true; expected = expected * 10 + 3;
                            if (mode == 3) { innerFailed = true; failure = "first-init"; }
                        }
                        if (failure.equals("none")) {
                            expected = expected * 10 + 4; sawInner = true;
                            if (mode == 4) failure = "body";
                        }
                    }
                } else if (failure.equals("none")) {
                    expected = expected * 10 + 5;
                    if (mode == 5) failure = "body";
                }
                if (failure.equals("none")) {
                    expected = expected * 10 + 6; sawOuter = true;
                    if (mode == 6) failure = "body";
                }
            }
            Outer result = null; Throwable thrown = null;
            try { result = ConditionalNestedCopiesCaller.run(variant, route, choose, tag, first, second); }
            catch (Throwable error) { thrown = error; }
            require(Trace.effects == expected, "ordered effects " + Trace.effects + " expected " + expected);
            if (failure.equals("none")) require(thrown == null && result == Trace.outer, "result identity");
            else if (failure.equals("body")) require(thrown == Trace.failure, "failure identity");
            else if (failure.equals("first-init")) require(thrown instanceof ExceptionInInitializerError
                && thrown.getCause() == Trace.failure, "initial initialization failure");
            else require(thrown instanceof NoClassDefFoundError, "repeated initialization failure");
            require((Trace.inner != null) == sawInner, "inner publication");
            require((Trace.outer != null) == sawOuter, "outer publication");
            if (sawInner) require(Trace.inner.tag == tag && Trace.inner.first == first
                && Trace.inner.second == second, "inner argument identity and bits");
            if (sawOuter) require(Trace.outer.tag == tag && Trace.outer.first == first
                && Trace.outer.second == second && Trace.outer.value == (choose ? Trace.inner : Trace.fallbackValue),
                "outer argument identity and bits");
            System.out.println("variant=" + variant + ",route=" + route + ",branch=" + (choose ? 1 : 0)
                + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.effects + ",failure=" + failure);
        }
    }
}
