import fixture.*;

public final class FinalInstanceNamesHarness {
    private static void check(boolean value) { if (!value) throw new AssertionError(); }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), receiverKind = Integer.parseInt(args[1]);
        int inputKind = Integer.parseInt(args[2]), mode = Integer.parseInt(args[3]);
        String spelling = args[4]; boolean synthetic = Boolean.parseBoolean(args[5]);
        check(Terminal.class.getDeclaredMethod(spelling, Object.class).isSynthetic() == synthetic);
        boolean failedInit = false;
        for (int attempt = 0; attempt < 2; ++attempt) {
            Trace.events = 0; Trace.seed = attempt == 0 ? 7 : 200;
            Trace.fail = mode == 1 ? 1 : mode == 2 ? 2 : mode == 3 ? 9 : 0;
            Trace.failure = new RuntimeException();
            Object initial = new Object(), value = new Object();
            Trace.initialized = initial; Trace.value = value;
            int[] values = {13}; Object input = inputKind == 0 ? values : inputKind == 1 ? null : value;
            long events = 0; int changed = 13, failureKind = 0;
            Object result = null, remaining = value;
            if (receiverKind == 0 && (failedInit || (attempt == 0 && mode == 3))) {
                failureKind = failedInit ? 6 : 5;
                if (!failedInit) { events = 9; remaining = initial; }
                failedInit = true;
            } else if (receiverKind == 1) failureKind = 1;
            else {
                int seed = attempt == 0 ? 100 : 200;
                if (attempt == 0) { events = 9; remaining = initial; }
                events = events * 11 + 1;
                if (mode == 1) failureKind = 3;
                else if (inputKind == 1) failureKind = 1;
                else if (inputKind == 2) failureKind = 2;
                else {
                    changed += seed; events = events * 11 + 2;
                    if (mode == 2) failureKind = 3;
                    else { result = remaining; remaining = null; }
                }
            }
            Throwable caught = null; Object actual = null;
            try {
                Terminal receiver = receiverKind == 0 ? new Terminal() : null;
                actual = FinalInstanceNamesCaller.call(route, spelling, receiver, input);
            } catch (Throwable failure) { caught = failure; }
            if (failureKind == 0) check(caught == null && actual == result);
            else if (failureKind == 1) check(caught instanceof NullPointerException);
            else if (failureKind == 2) check(caught instanceof ClassCastException);
            else if (failureKind == 3) check(caught == Trace.failure);
            else if (failureKind == 5) {
                check(caught instanceof ExceptionInInitializerError);
                check(caught.getCause() == Trace.failure);
            } else check(caught instanceof NoClassDefFoundError);
            check(values[0] == changed && Trace.events == events && Trace.value == remaining);
            Object alien = new Object();
            check(Alien.class.getDeclaredMethod("read-value", Object.class).invoke(new Alien(), alien) == alien);
        }
        System.out.println("attempts=2,results-identities-effects-failures=matched,dependency-name=retained");
    }
}
