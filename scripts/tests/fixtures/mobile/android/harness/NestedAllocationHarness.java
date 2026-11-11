import fixture.*;
public final class NestedAllocationHarness {
    static void require(boolean ok) { if (!ok) throw new AssertionError("nested allocation order"); }
    static final class Derived extends Provider {
        int entries;
        public Object read() { entries++; return super.read(); }
    }
    static final class Problem {
        int calls;
        RuntimeException failure;
        public String toString() { calls++; if (failure != null) throw failure; return "payload"; }
    }
    public static void main(String[] args) throws Throwable {
        if (args[0].equals("linear")) {
            int mode = Integer.parseInt(args[1]), route = Integer.parseInt(args[2]);
            Provider provider = mode == 7 ? null : mode == 9 ? new Derived() : new Provider();
            Object token = mode == 8 ? null : new Object();
            if (provider != null) provider.stored = token;
            RuntimeException original = new IllegalArgumentException("owned linear allocation failure");
            Trace.failure = original; Trace.failAt = mode;
            Inner previous = null;
            long events = 0;
            int reads = 0, inners = 0;
            for (int attempt = 1; attempt <= 2; attempt++) {
                boolean success = mode == 0 || mode == 8 || mode == 9;
                if (mode == 2) events = 2;
                else {
                    if (attempt == 1) events = 2;
                    if (mode != 7) {
                        events = events * 10 + 3; reads++;
                        if (mode != 3) { events = events * 10 + 4; inners++; }
                    }
                }
                try {
                    Inner result = route == 0 ? Creator.read(provider) : NestedAllocationCaller.read(provider);
                    require(success && result != previous && result.value == token);
                    previous = result;
                } catch (Throwable error) {
                    require(!success);
                    if (mode == 2) {
                        if (attempt == 1) require(error.getClass() == ExceptionInInitializerError.class && error.getCause() == original);
                        else require(error.getClass() == NoClassDefFoundError.class);
                    } else if (mode == 7) require(error.getClass() == NullPointerException.class);
                    else require(error == original);
                }
                require(Trace.events == events && Trace.reads == reads && Trace.inners == inners);
                require(Trace.outers == 0 && Trace.arrays == 0);
                if (mode == 9) require(((Derived) provider).entries == attempt);
            }
            System.out.println("linear=" + mode + ",route=" + route + ",trace=" + Trace.events); return;
        }
        if (args[0].equals("frames")) {
            int values = 0, failures = 0;
            for (int route = 0; route < 2; route++) for (String label : new String[]{null, "", "unicode-\u03bb"}) {
                for (String file : new String[]{null, "source-\u03bb.java"})
                    for (int line : new int[]{Integer.MIN_VALUE, -2, -1, 0, 1, Integer.MAX_VALUE}) {
                        StackTraceElement original = new StackTraceElement("owned.Original", "source", file, line);
                        StackTraceElement result = route == 0 ? Creator.frame(original, label) : NestedAllocationCaller.frame(original, label);
                        require(result != original && result.getClassName().equals("owned." + label));
                        require(result.getMethodName().equals("frame") && java.util.Objects.equals(result.getFileName(), file));
                        require(result.getLineNumber() == line); values++;
                    }
                try { if (route == 0) Creator.frame(null, label); else NestedAllocationCaller.frame(null, label); throw new AssertionError("missing failure"); }
                catch (NullPointerException expected) { failures++; }
            }
            System.out.println("integer-results=" + values + ",null-receiver-failures=" + failures); return;
        }
        if (args[0].equals("cast")) {
            int mode = Integer.parseInt(args[1]), route = Integer.parseInt(args[2]);
            Object input = mode == 1 ? null : mode == 2 || mode == 3 ? new Object() : "owned";
            RuntimeException original = new IllegalStateException("owned cast allocation failure");
            Trace.failure = original; Trace.failAt = mode == 3 ? 2 : mode == 4 ? 4 : 0;
            Inner previous = null;
            for (int attempt = 1; attempt <= 2; attempt++) {
                try {
                    Inner result = route == 0 ? Creator.checked(input) : NestedAllocationCaller.checked(input);
                    require(mode <= 1 && result != previous && result.value == input); previous = result;
                } catch (Throwable problem) {
                    if (mode == 2) require(problem.getClass() == ClassCastException.class);
                    else if (mode == 3) {
                        if (attempt == 1) require(problem.getClass() == ExceptionInInitializerError.class && problem.getCause() == original);
                        else require(problem.getClass() == NoClassDefFoundError.class);
                    } else require(mode == 4 && problem == original);
                }
                require(Trace.events == (mode == 2 || mode == 3 ? 2 : attempt == 1 ? 24 : 244));
                require(Trace.inners == (mode == 2 || mode == 3 ? 0 : attempt));
            }
            System.out.println("cast=" + mode + ",route=" + route + ",trace=" + Trace.events); return;
        }
        if (args[0].equals("strings")) {
            for (int route = 0; route < 2; route++) {
                IllegalStateException literal = route == 0 ? Creator.literal() : NestedAllocationCaller.literal();
                IllegalStateException another = route == 0 ? Creator.literal() : NestedAllocationCaller.literal();
                require(literal != another && literal.getMessage().equals("owned literal") && another.getMessage().equals("owned literal"));
                Problem problem = new Problem();
                IllegalStateException first = route == 0 ? Creator.failure(problem) : NestedAllocationCaller.failure(problem);
                require(first.getClass() == IllegalStateException.class && first.getMessage().equals("owned payload") && problem.calls == 1);
                IllegalStateException second = route == 0 ? Creator.failure(null) : NestedAllocationCaller.failure(null);
                require(second != first && second.getMessage().equals("owned null") && problem.calls == 1);
                problem.failure = new UnsupportedOperationException("owned toString failure");
                try { if (route == 0) Creator.failure(problem); else NestedAllocationCaller.failure(problem); throw new AssertionError("missing failure"); }
                catch (UnsupportedOperationException failure) { require(failure == problem.failure && problem.calls == 2); }
            }
            System.out.println("string-builder-chains=matched,constructions=8,ordered-failures=2"); return;
        }
        int mode = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]);
        Provider provider = mode == 7 ? null : mode == 9 ? new Derived() : new Provider();
        Object token = mode == 8 ? null : new Object(); if (provider != null) provider.stored = token;
        RuntimeException original = new IllegalArgumentException("owned nested failure");
        Trace.failure = original; Trace.failAt = mode;
        Outer<?> previous = null;
        for (int attempt = 1; attempt <= 2; attempt++) {
            boolean success = mode == 0 || mode == 8 || mode == 9;
            try {
                Outer<?> result = route == 0 ? Creator.make(provider, 3) : NestedAllocationCaller.make(provider, 3);
                require(success && result != previous && result.inner.value == token && result.payload == token);
                require(result.bytes.length == 3 && result.bytes[0] == 0);
                if (previous != null) require(result.inner != previous.inner && result.bytes != previous.bytes);
                previous = result;
            } catch (Throwable problem) {
                require(!success);
                if (mode == 7) require(problem.getClass() == NullPointerException.class);
                else if (mode <= 2) {
                    if (attempt == 1) require(problem.getClass() == ExceptionInInitializerError.class && problem.getCause() == original);
                    else require(problem.getClass() == NoClassDefFoundError.class);
                } else require(problem == original);
            }
            long trace = mode == 1 ? 1 : mode == 2 || mode == 7 ? 12
                : mode == 3 ? (attempt == 1 ? 123 : 1233)
                : mode == 4 ? (attempt == 1 ? 1234 : 123434)
                : mode == 5 ? (attempt == 1 ? 12345 : 12345345)
                : (attempt == 1 ? 123456 : 1234563456L);
            require(Trace.events == trace);
            if (mode == 9) require(((Derived)provider).entries == attempt);
            require(Trace.reads == (mode <= 2 && mode != 0 || mode == 7 ? 0 : attempt));
            require(Trace.inners == (mode == 0 || mode >= 4 && mode != 7 ? attempt : 0));
            require(Trace.arrays == (mode == 0 || mode >= 5 && mode != 7 ? attempt : 0));
            require(Trace.outers == (mode == 0 || mode == 6 || mode == 8 || mode == 9 ? attempt : 0));
        }
        System.out.println("mode=" + mode + ",route=" + route + ",trace=" + Trace.events);
    }
}
