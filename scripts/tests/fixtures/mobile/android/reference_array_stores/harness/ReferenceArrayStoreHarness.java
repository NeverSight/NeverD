import fixture.*;
public final class ReferenceArrayStoreHarness {
    static void require(boolean value, String detail) {
        if (!value) throw new AssertionError("reference array store contract: " + detail);
    }
    public static void main(String[] args) throws Throwable {
        int variant = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        boolean nested = variant == 2, incoming = variant == 0 || variant == 3, rows = variant == 3, loop = variant == 4;
        boolean targetStarted = false, targetFailed = false;
        for (int attempt = 0; attempt < 2; ++attempt) {
            Trace.fail = mode == 8 ? 1 : mode == 9 ? 3 : mode == 10 ? 4 : mode == 11 ? 5 : mode == 12 ? 2 : 0;
            Trace.reads = 0; Trace.effects = 0; Trace.array = null; Trace.seen = null; Trace.target = null;
            Trace.failure = new IllegalStateException("owned store failure");
            Object seed = rows ? new int[]{11} : "seed";
            Object valid = rows ? new int[]{22} : "value";
            Object value = mode == 1 ? null : mode == 2 || mode == 3 || mode == 4 || mode == 5 || mode == 6 || mode == 13 ? new Object() : valid;
            int length = mode == 6 ? -1 : mode == 5 || mode == 7 || mode == 9 ? 0 : 2;
            int index = mode == 3 ? -1 : mode == 4 ? 2 : mode == 13 ? 1 : 0;
            Object[] array = mode == 5 || mode == 9 ? null : mode == 6 ? new Object[]{seed, seed}
                : rows ? (Object[])new int[mode == 7 ? 0 : 2][] : new String[mode == 7 ? 0 : 2];
            if (array != null && array.length != 0) { array[0] = seed; array[1] = seed; }
            Object[] values = mode == 5 ? null : mode == 6 || mode == 7 ? new Object[0]
                : new Object[]{mode == 1 ? null : mode == 2 ? new Object() : "first", mode == 13 ? new Object() : "second"};
            Object tag = new Object();
            long expected = 0;
            Class<?> expectedError = null;
            boolean bodyFailure = false, publishedTarget = false;
            int expectedReads = 0, stores = 0;
            Object expectedSeen = null;
            if (nested) {
                if (targetFailed) expectedError = NoClassDefFoundError.class;
                else if (!targetStarted) {
                    targetStarted = true; expected = 1;
                    if (Trace.fail == 1) { targetFailed = true; expectedError = ExceptionInInitializerError.class; }
                }
            }
            if (expectedError == null && incoming && mode == 6) expectedError = ClassCastException.class;
            if (expectedError == null && loop && values == null) expectedError = NullPointerException.class;
            if (expectedError == null) {
                expected = expected * 10 + 2;
                if (Trace.fail == 2) { expectedError = IllegalStateException.class; bodyFailure = true; }
            }
            if (expectedError == null && !incoming && !loop && length < 0) expectedError = NegativeArraySizeException.class;
            int count = loop ? values == null ? 0 : values.length : 1;
            for (int i = 0; i < count && expectedError == null; ++i) {
                expected = expected * 10 + 3; ++expectedReads;
                expectedSeen = loop ? values[i] : value;
                if (Trace.fail == 3) { expectedError = IllegalStateException.class; bodyFailure = true; }
                else if (incoming && array == null) expectedError = NullPointerException.class;
                else if (!loop && (index < 0 || index >= (incoming ? array.length : length))) expectedError = ArrayIndexOutOfBoundsException.class;
                else if (expectedSeen != null && !(rows ? expectedSeen instanceof int[] : expectedSeen instanceof String)) expectedError = ArrayStoreException.class;
                else {
                    ++stores;
                    if (nested) {
                        expected = expected * 10 + 5; publishedTarget = true;
                        if (Trace.fail == 5) { expectedError = IllegalStateException.class; bodyFailure = true; }
                    }
                    if (expectedError == null) {
                        expected = expected * 10 + 4;
                        if (Trace.fail == 4) { expectedError = IllegalStateException.class; bodyFailure = true; }
                    }
                }
            }
            Object result = null; Throwable error = null;
            try { result = ReferenceArrayStoreCaller.run(variant, route, array, value, length, index, tag, values); }
            catch (Throwable caught) { error = caught; }
            require(Trace.effects == expected, "effects " + Trace.effects + " expected " + expected);
            require(expectedError == null ? error == null : error != null && error.getClass() == expectedError, "exception class");
            if (bodyFailure) require(error == Trace.failure, "body failure identity");
            if (expectedError == ExceptionInInitializerError.class) require(error.getCause() == Trace.failure, "initializer failure identity");
            require(Trace.reads == expectedReads && Trace.seen == expectedSeen, "original read count/value");
            require((Trace.target != null) == publishedTarget, "partial constructor publication");
            Object[] observed = expectedReads == 0 ? null : Trace.array;
            if (incoming && expectedReads != 0) require(observed == array, "incoming array identity");
            if (!incoming && observed != null) require(observed.getClass() == String[].class, "original runtime component");
            if (incoming && array != null) {
                for (int i = 0; i < array.length; ++i)
                    require(array[i] == (stores != 0 && i == index ? value : seed), "incoming partial store");
            } else if (observed != null) {
                require(observed.length == (loop ? values.length : length), "original length");
                for (int i = 0; i < observed.length; ++i)
                    require(observed[i] == (loop ? i < stores ? values[i] : null : stores != 0 && i == index ? value : null), "fresh partial store");
            }
            if (publishedTarget) require(Trace.target.array == observed && Trace.target.stored == value && Trace.target.tag == tag, "constructor argument identities");
            if (expectedError == null) {
                if (nested) require(result == Trace.target, "target result identity");
                else if (expectedReads != 0) require(result == observed, "array result identity");
                else require(result instanceof String[] && ((String[])result).length == 0, "empty loop result");
            }
            System.out.println("variant=" + variant + ",route=" + route + ",mode=" + mode + ",attempt=" + attempt
                + ",trace=" + Trace.effects + ",stores=" + stores + ",error=" + (error == null ? "none" : error.getClass().getSimpleName()));
        }
    }
}
