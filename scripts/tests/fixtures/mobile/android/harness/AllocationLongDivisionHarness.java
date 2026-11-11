import fixture.*;
import java.lang.reflect.InvocationTargetException;
import java.math.BigInteger;

public final class AllocationLongDivisionHarness {
    static void require(boolean ok, String detail) {
        if (!ok) throw new AssertionError("owned long division allocation: " + detail);
    }
    static Target call(int route) throws Throwable {
        try {
            if (route == 0) return Core.make();
            if (route == 1) return AllocationLongDivisionCaller.call();
            if (route == 2) return (Target) Core.class.getMethod("make").invoke(null);
            return Core.caught();
        } catch (InvocationTargetException failure) { throw failure.getCause(); }
    }
    static long mutatedLeft(int attempt) { return -0x123456789abcdefL - attempt; }
    static final class Expected {
        long events, left, right;
        int steps, failAt, divisor, mode, attempt;
        boolean failed, original, arithmetic, caught, published, firstFields, lastFields;
        long[] values = new long[6];
        void hit(int event) {
            events = events * 10 + event;
            steps++;
            if ((mode == 24 && steps == 2) || (mode == 25 && steps == 6)) left = mutatedLeft(attempt);
            if ((mode == 28 && steps == 10) || (mode == 29 && steps == 12)) right = 0;
            if (event == failAt) { failed = true; original = true; arithmetic = mode == 26; }
        }
        void compute(int route) {
            for (int i = 0; i < values.length && !failed; i++) {
                hit(1);
                if (failed) break;
                long numerator = left;
                hit(i == 2 || i == 3 ? 3 : 2);
                if (failed) break;
                long denominator = i == 2 || i == 3 ? divisor : right;
                if (denominator == 0) { failed = true; arithmetic = true; break; }
                BigInteger a = BigInteger.valueOf(numerator), b = BigInteger.valueOf(denominator);
                values[i] = (i % 2 == 0 ? a.divide(b) : a.remainder(b)).longValue();
                if (i >= 4) values[i] = 0;
            }
            if (!failed) { published = true; hit(4); }
            if (!failed) { firstFields = true; hit(5); }
            if (!failed) { lastFields = true; hit(6); }
            if (!failed) hit(7);
            if (!failed) hit(8);
            if (arithmetic && route == 3) { caught = true; hit(9); }
        }
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        long[] lefts = {Long.MIN_VALUE, Long.MAX_VALUE, 0, 1, -1, 0x100000001L,
            0x8123456789abcdefL, 0x76543210fedcba98L, Long.MIN_VALUE, 19, -19, 19, -19, 123, 456, 0};
        long[] rights = {-1, 1, -1, -1, Long.MIN_VALUE, 3, -7, 0x100000001L, 2, 3, 3, -3, -3, 0, 7, 0};
        int[] divisors = {-1, -1, 1, -1, Integer.MIN_VALUE, 3, -7, Integer.MAX_VALUE, 2, 3, 3, -3, -3, 3, 0, 0};
        Target previous = null;
        for (int attempt = 0; attempt < 2; attempt++) {
            int edge = (mode + attempt) % lefts.length;
            Expected expected = new Expected();
            expected.mode = mode; expected.attempt = attempt;
            expected.left = mode < 16 ? lefts[edge] : 0x7fedcba987654321L - attempt;
            expected.right = mode < 16 ? rights[edge] : -7;
            expected.divisor = mode < 16 ? divisors[edge] : 3;
            expected.failAt = mode >= 16 && mode < 24 ? mode - 15 : mode == 26 ? 2 : mode == 27 ? 9 : 0;
            if (mode == 27) expected.right = 0;
            RuntimeException original = mode == 26 ? new ArithmeticException("owned provider failure")
                : new IllegalStateException("owned provider or cleanup failure");
            Trace.events = 0; Trace.steps = 0; Trace.partial = null; Trace.caught = null;
            Trace.left = expected.left; Trace.right = expected.right; Trace.divisor = expected.divisor;
            Trace.failAt = expected.failAt; Trace.failure = original;
            final int currentAttempt = attempt;
            Trace.observer = () -> {
                require(Trace.initializations == 12, "class initialization order");
                if ((mode == 24 && Trace.steps == 2) || (mode == 25 && Trace.steps == 6)) Trace.left = mutatedLeft(currentAttempt);
                if ((mode == 28 && Trace.steps == 10) || (mode == 29 && Trace.steps == 12)) Trace.right = 0;
            };
            expected.compute(route);
            Target actual = null;
            Throwable thrown = null;
            try { actual = call(route); } catch (Throwable failure) { thrown = failure; }
            require((thrown != null) == expected.failed, "exception presence");
            if (expected.failed) {
                if (expected.original) require(thrown == original, "original exception identity");
                else require(thrown.getClass() == ArithmeticException.class, "divide by zero exception");
                require(actual == null, "result on failure");
            }
            require((Trace.caught != null) == expected.caught, "handler entry");
            if (expected.caught) {
                require(Trace.caught.getClass() == ArithmeticException.class, "handler exception type");
                if (mode == 26) require(Trace.caught == original, "caught provider exception identity");
                else if (mode != 27) require(Trace.caught == thrown, "rethrow identity");
                else require(Trace.caught != original, "cleanup keeps original divide failure");
            }
            require(Trace.events == expected.events && Trace.steps == expected.steps, "effect order");
            require(Trace.left == expected.left && Trace.right == expected.right, "callback state");
            require((Trace.partial != null) == expected.published, "partial object publication");
            if (expected.published) {
                Pair partial = (Pair) Trace.partial;
                long[] actualValues = {partial.a, partial.b, partial.c, partial.d, partial.e, partial.f};
                for (int i = 0; i < actualValues.length; i++) {
                    long wanted = (i < 3 ? expected.firstFields : expected.lastFields) ? expected.values[i] : 0;
                    require(actualValues[i] == wanted, "partial field " + i);
                }
                if (!expected.failed) require(actual != previous && actual.pair == partial, "allocation identity");
            }
            previous = actual;
        }
        System.out.println("attempts=2,long-division-allocation=matched");
    }
}
