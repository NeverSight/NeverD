import fixture.*;
public final class TrailingAllocationAbortHarness {
    static void check(boolean condition) {
        if (!condition) throw new AssertionError("owned trailing allocation abort");
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        for (int attempt = 0; attempt < 2; attempt++) {
            Object payload = mode == 1 ? null : mode == 2 ? new Object[1] : new Object();
            Checked checked = new Checked(payload);
            boolean abort = mode == 3 || mode == 4 || mode == 5 || mode >= 14;
            Object initial = mode == 4 ? null : abort && mode != 5 ? new Object() : checked;
            Object late = new Object(), style = mode == 1 ? null : new Object();
            Provider.current = initial; Provider.late = late; Provider.style = style;
            Trace.events = 0; Trace.seen = null; Trace.inner = null; Trace.outer = null; Trace.aborted = null; Trace.styleSeen = null;
            Trace.failure = new IllegalArgumentException("owned failure"); Trace.error = new LinkageError("owned error");
            Trace.failAt = mode == 6 ? 3 : mode == 7 ? -3 : mode == 8 ? 4 : mode == 9 ? -4
                : mode == 10 ? 5 : mode == 11 ? -5 : mode == 12 ? 6 : mode == 13 ? -6 : mode == 14 ? 8 : mode == 15 ? -8 : 0;
            int firstBits = mode == 1 ? 0x80000000 : mode == 2 ? 0x7fc01234 : 0x3fc00000;
            int secondBits = mode == 2 ? 0xff800000 : 0x7f800000;
            float first = Float.intBitsToFloat(firstBits), second = Float.intBitsToFloat(secondBits);
            Outer result = null; Throwable caught = null;
            try { result = TrailingAllocationAbortCaller.call(route, first, second, mode == 5 ? 2 : 0); }
            catch (Throwable failure) { caught = failure; }
            int events = attempt == 0 ? 12 : 0;
            if (abort) {
                if (attempt == 0) events = events * 10 + 7;
                events = events * 10 + 8;
                check(Trace.seen == null && Trace.styleSeen == null && Trace.inner == null && Trace.outer == null);
                check(Trace.aborted != null && Trace.aborted.getMessage().equals(mode == 5 ? "owned mode" : "owned input"));
                if (mode >= 14) check(caught == (mode == 14 ? Trace.failure : Trace.error));
                else check(caught == Trace.aborted);
                check(Provider.current == initial && result == null);
            } else {
                events = events * 10 + 3;
                boolean styled = mode != 6 && mode != 7;
                boolean constructed = styled && mode != 8 && mode != 9;
                boolean outer = constructed && mode != 10 && mode != 11;
                if (styled) events = events * 10 + 4;
                if (constructed) events = events * 10 + 5;
                if (outer) events = events * 10 + 6;
                check(Trace.seen == checked && Provider.current == late && Trace.aborted == null);
                check(Trace.styleSeen == (styled ? checked : null));
                check((Trace.inner != null) == constructed && (Trace.outer != null) == outer);
                if (constructed) {
                    check(Trace.inner.value == payload && Trace.inner.style == style);
                    check(Float.floatToRawIntBits(Trace.inner.first) == firstBits && Float.floatToRawIntBits(Trace.inner.second) == secondBits);
                }
                if (outer) check(Trace.outer.value == Trace.inner);
                if (mode >= 6) check(caught == (Trace.failAt > 0 ? Trace.failure : Trace.error) && result == null);
                else check(caught == null && result == Trace.outer && result.value == Trace.inner);
            }
            check(Trace.events == events);
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",events=" + events);
        }
    }
}
