import fixture.*;
public final class ForwardAllocationHarness {
    static void require(boolean condition) {
        if (!condition) throw new AssertionError("owned forward allocation contract");
    }
    public static void main(String[] args) {
        int route = Integer.parseInt(args[0]), selector = Integer.parseInt(args[1]);
        int mode = Integer.parseInt(args[2]), kind = Integer.parseInt(args[3]);
        boolean allocates = selector != 0 && selector != 1;
        Throwable sentinel = kind == 0 ? new IllegalArgumentException("owned failure") : new LinkageError("owned failure");
        Throwable cleanup = kind == 0 ? new IllegalStateException("owned cleanup") : new AssertionError("owned cleanup");
        Target previous = null;
        for (int attempt = 0; attempt < 2; ++attempt) {
            Object seed = new Object(), initialized = new Object(), late = new Object();
            Trace.events = 0; Trace.value = seed; Trace.partial = null; Trace.caught = null;
            Trace.hook = event -> {
                if (event == 1) Trace.value = initialized;
                if (event == 2) Trace.value = late;
                Throwable failure = event == mode || mode == 6 && event == 2 ? sentinel
                    : mode == 6 && event == 3 ? cleanup : null;
                if (failure instanceof RuntimeException) throw (RuntimeException) failure;
                if (failure != null) throw (Error) failure;
            };
            Target result = null; Throwable caught = null;
            try { result = route == 0 ? Client.make(selector) : ForwardAllocationCaller.make(selector); }
            catch (Throwable failure) { caught = failure; }
            boolean initFailure = allocates && mode == 1;
            boolean injected = mode == 5 || selector == 0 && (mode == 3 || mode == 4)
                || allocates && (mode == 2 || mode == 6);
            boolean handled = !initFailure && (selector == 0 && mode != 4 && mode != 5 || injected && kind == 0);
            if (initFailure) {
                if (attempt == 1) require(caught instanceof NoClassDefFoundError);
                else if (kind == 0) require(caught instanceof ExceptionInInitializerError && caught.getCause() == sentinel);
                else require(caught == sentinel);
            } else if (mode == 6 && kind == 0) require(caught == cleanup);
            else if (mode == 3 || injected && kind == 1) require(caught == sentinel);
            else require(caught == null);
            if (handled) {
                if (mode == 5 || mode == 4 || mode == 2 || mode == 6) require(Trace.caught == sentinel);
                else require(Trace.caught instanceof IllegalStateException && Trace.caught.getMessage().equals("owned sibling"));
            } else require(Trace.caught == null);
            long expected = 5;
            if (mode != 5) {
                if (selector == 0) expected = 54;
                else if (allocates) {
                    if (attempt == 0) expected = 51;
                    if (!initFailure) expected = expected * 10 + 2;
                }
            }
            if (handled) expected = expected * 10 + 3;
            require(Trace.events == expected);
            boolean constructed = allocates && mode != 1 && mode != 5;
            if (constructed) {
                require(Trace.partial != null && Trace.partial != previous);
                require(Trace.partial.first == (attempt == 0 ? initialized : seed));
                if (mode == 0) require(result == Trace.partial);
                previous = Trace.partial;
            } else require(Trace.partial == null && result == null);
            System.out.println("route=" + route + ",selector=" + selector + ",mode=" + mode + ",error=" + kind + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
