import fixture.*;

public final class LiteralSuperclassBridgesHarness {
    private static void check(boolean value) { if (!value) throw new AssertionError(); }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), receiverKind = Integer.parseInt(args[1]);
        int valueKind = Integer.parseInt(args[2]), mode = Integer.parseInt(args[3]);
        boolean failedInit = false;
        for (int attempt = 0; attempt < 2; ++attempt) {
            Trace.events = 0; Trace.seed = attempt == 0 ? 7 : 200;
            Trace.fail = mode == 3 ? 1 : mode == 4 ? 2 : mode == 5 ? 9 : 0;
            Trace.failure = new RuntimeException();
            Object sentinel = new Object(); Trace.last = sentinel;
            Trace.number = 0x12345678deadbeefL;
            int[] values = {13}; Object note = mode == 1 ? null : mode == 2 ? sentinel : values;
            Wide value = valueKind == 0 ? new Narrow() : valueKind == 1 ? null : new Wide();
            long events = 0; int changed = 13; Object last = sentinel;
            long number = Trace.number;
            int failureKind = 0;
            if (receiverKind != 2 && (failedInit || (attempt == 0 && mode == 5))) {
                failureKind = failedInit ? 6 : 5;
                if (!failedInit) events = 9;
                failedInit = true;
            } else {
                if (receiverKind != 2 && attempt == 0) events = 9;
                int seed = receiverKind != 2 && attempt == 0 ? 100 : Trace.seed;
                events = events * 11 + 8;
                if (note == values) changed += 2;
                if (receiverKind == 2) failureKind = route == 0 && valueKind == 2 ? 2 : 1;
                else if (valueKind == 2) failureKind = 2;
                else {
                    if (receiverKind == 1) events = events * 11 + 3;
                    events = events * 11 + 1;
                    if (mode == 3) failureKind = 3;
                    else if (mode == 1) failureKind = 1;
                    else if (mode == 2) failureKind = 2;
                    else {
                        changed += seed; last = value;
                        number = 0x87654321abcdef09L ^ seed;
                        events = events * 11 + 2;
                        if (mode == 4) failureKind = 3;
                    }
                }
            }
            Throwable caught = null;
            try {
                Core receiver = receiverKind == 2 ? null : receiverKind == 0
                    ? new Core() : new LiteralSuperclassBridgesCaller.Child();
                LiteralSuperclassBridgesCaller.call(route, receiver, value, note);
            } catch (Throwable failure) { caught = failure; }
            if (failureKind == 0) check(caught == null);
            else if (failureKind == 1) check(caught instanceof NullPointerException);
            else if (failureKind == 2) check(caught instanceof ClassCastException);
            else if (failureKind == 3) check(caught == Trace.failure);
            else if (failureKind == 5) {
                check(caught instanceof ExceptionInInitializerError);
                check(caught.getCause() == Trace.failure);
            } else check(caught instanceof NoClassDefFoundError);
            check(Trace.events == events && values[0] == changed);
            check(Trace.last == last && Trace.number == number);
        }
        System.out.println("attempts=2,dispatch-casts-wide-effects-failures=matched");
    }
}
