import fixture.*;
public final class SwitchAllocationHarness {
    static void require(boolean condition) {
        if (!condition) throw new AssertionError("owned switch allocation contract");
    }
    public static void main(String[] args) {
        int route = Integer.parseInt(args[0]), selector = Integer.parseInt(args[1]);
        int mode = Integer.parseInt(args[2]), errorKind = Integer.parseInt(args[3]);
        Throwable sentinel = errorKind == 0 ? new IllegalArgumentException("owned failure") : new LinkageError("owned failure");
        boolean valid = selector >= 0 && selector <= 2;
        Target previous = null;
        for (int attempt = 0; attempt < 2; ++attempt) {
            Object seed = mode == 11 ? null : new Object();
            Object before = mode == 12 ? null : new Object(), afterInit = mode == 12 ? null : new Object();
            Object lateFirst = new Object(), lateSecond = new Object();
            Trace.events = 0; Trace.partial = null; Trace.selected = -999;
            Trace.first = seed; Trace.second = before;
            Trace.hook = event -> {
                if (event == 1) Trace.first = lateFirst;
                if (event == 3) Trace.second = afterInit;
                if (event == 4 || event == 6) Trace.second = lateSecond;
                if (event == mode) {
                    if (sentinel instanceof RuntimeException) throw (RuntimeException) sentinel;
                    throw (Error) sentinel;
                }
            };
            Target result = null;
            Throwable caught = null;
            try { result = route == 0 ? Creator.make(Trace.first, selector) : SwitchAllocationCaller.make(Trace.first, selector); }
            catch (Throwable failure) { caught = failure; }
            boolean initFailure = mode == 3;
            if (initFailure) {
                if (attempt == 1) require(caught instanceof NoClassDefFoundError);
                else if (errorKind == 0) require(caught instanceof ExceptionInInitializerError && caught.getCause() == sentinel);
                else require(caught == sentinel);
            } else if (mode > 0 && mode < 9) require(caught == sentinel);
            else if (!valid) require(caught instanceof IllegalStateException && caught.getMessage().equals("owned selector " + selector));
            else require(caught == null);
            int[] events = valid ? new int[] {1,2,3,4,6,8} : new int[] {1,2,3,7};
            long expected = 0;
            for (int event : events) {
                if (attempt == 1 && event == 3) {
                    if (initFailure) break;
                    continue;
                }
                expected = expected * 10 + event;
                if (event == mode) break;
            }
            require(Trace.events == expected);
            boolean constructed = valid && mode != 1 && mode != 2 && mode != 3 && mode != 4;
            Object choice = attempt == 0 ? afterInit : before;
            if (constructed) {
                require(Trace.partial != null && Trace.partial != previous && Trace.partial.first == seed);
                require(Trace.partial.second == (mode == 6 ? null : choice));
                if (mode != 6 && mode != 8) require(result == Trace.partial);
                previous = Trace.partial;
            } else require(Trace.partial == null && result == null);
            boolean called = valid && mode != 1 && mode != 2 && mode != 3;
            require(Trace.selected == (called ? selector : -999));
            System.out.println("route=" + route + ",selector=" + selector + ",mode=" + mode + ",error=" + errorKind + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
