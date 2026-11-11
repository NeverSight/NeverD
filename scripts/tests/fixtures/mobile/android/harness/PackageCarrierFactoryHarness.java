import fixture.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
import java.io.Serializable;
public final class PackageCarrierFactoryHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned package carrier factory");
    }
    static int reads;
    static final class Trap implements Seed<String> {
        public String read() { reads++; return "owned value"; }
        public boolean equals(Object other) { throw new AssertionError("owned package carrier factory eager equals"); }
        public int hashCode() { throw new AssertionError("owned package carrier factory eager hash"); }
        public String toString() { throw new AssertionError("owned package carrier factory eager string"); }
    }
    static final class Reply extends Factory<Object> implements Serializable {}
    static Object call(int route, int operation, Seed seed, int code, Context first, Context second) throws Throwable {
        if (route == 0) return operation == 0 ? Client.call(seed, code, first, second) : Client.checked(seed, code, first, second);
        if (route == 1) return PackageCarrierFactoryCaller.call(operation, seed, code, first, second);
        String name = operation == 0 ? "call" : "checked";
        if (route == 2) {
            try { return Client.class.getMethod(name, Seed.class, int.class, Context.class, Context.class).invoke(null, seed, code, first, second); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        MethodHandle handle = MethodHandles.publicLookup().findStatic(Client.class, name,
            MethodType.methodType(operation == 0 ? Factory.class : Serializable.class, Seed.class, int.class, Context.class, Context.class));
        return operation == 0 ? (Factory)handle.invokeExact(seed, code, first, second) : (Serializable)handle.invokeExact(seed, code, first, second);
    }
    @SuppressWarnings("unchecked")
    private static <E extends Throwable> void fail(Throwable failure) throws E { throw (E) failure; }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), operation = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        for (Method method : Factory.class.getDeclaredMethods()) {
            if (!method.getName().equals("select")) continue;
            int flags = method.getModifiers();
            require(Modifier.isStatic(flags) && !Modifier.isPublic(flags) && !Modifier.isPrivate(flags) && !Modifier.isProtected(flags));
        }
        final Seed seed = mode == 5 ? null : mode == 6 ? new Seed<Object>() {
            public Object read() { reads++; return new Object(); }
        } : new Trap();
        final int code = mode % 2 == 0 ? Integer.MIN_VALUE : Integer.MAX_VALUE;
        final Context first = mode == 13 ? null : new Context();
        final Context second = mode == 9 ? first : new Context();
        final Context passedSecond = operation == 0 ? second : null;
        final Object contextValue = new Object();
        if (first != null) first.value = new Object();
        final Object expected = mode == 7 ? null : mode == 9 ? new Factory<Object>()
            : mode == 10 ? new Object() : mode == 11 ? new Base<Object>() {} : new Reply();
        final RuntimeException runtimeFailure = new RuntimeException("owned callback failure");
        final java.io.IOException checkedFailure = new java.io.IOException();
        final Error errorFailure = new Error("owned callback error");
        Trace.failure = new RuntimeException("owned stage failure");
        Trace.failAt = mode == 1 ? 1 : mode == 2 ? 2 : mode == 8 ? 3 : 0;
        Trace.action = new Runnable() { public void run() {
            require(Trace.input == seed && Trace.events == 12);
            require(Trace.code == code && Trace.first == first && Trace.second == passedSecond && reads == 0);
            Trace.hit(4); Trace.result = expected;
            if (first != null) first.value = contextValue;
            if (mode == 12) PackageCarrierFactoryHarness.<RuntimeException>fail(checkedFailure);
            if (mode == 3) throw runtimeFailure;
            if (mode == 4) throw errorFailure;
        }};
        for (int attempt = 0; attempt < 2; ++attempt) {
            Object sentinel = new Object();
            Trace.events = 0; Trace.input = sentinel; Trace.result = seed;
            Trace.code = 0; Trace.first = null; Trace.second = null; reads = 0;
            Throwable failure = null; Object actual = sentinel;
            try { actual = call(route, operation, seed, code, first, second); } catch (Throwable error) { failure = error; }
            require(Trace.events == (mode == 1 ? 1 : mode == 2 ? 12 : mode == 3 || mode == 4 || mode == 12 ? 124 : 1243));
            require(Trace.input == (mode == 1 ? sentinel : seed) && reads == 0);
            require(Trace.code == (mode == 1 ? 0 : code));
            require(Trace.first == (mode == 1 ? null : first) && Trace.second == (mode == 1 ? null : passedSecond));
            if (mode != 1 && mode != 2 && first != null) require(first.value == contextValue);
            require(Trace.result == (mode == 1 || mode == 2 ? seed : expected));
            Throwable wanted = mode == 1 || mode == 2 || mode == 8 ? Trace.failure
                : mode == 3 ? runtimeFailure : mode == 4 ? errorFailure : mode == 12 ? checkedFailure : null;
            boolean castFailure = mode == 10 || mode == 11 || operation == 1 && mode == 9;
            if (castFailure) require(failure instanceof ClassCastException);
            else require(failure == wanted);
            if (wanted == null && !castFailure) require(actual == expected);
            else require(actual == sentinel);
            if (seed != null) {
                if (mode == 6) {
                    boolean cast = false;
                    try { String ignored = ((Seed<String>) seed).read(); } catch (ClassCastException failureAfterCall) { cast = true; }
                    require(cast && reads == 1);
                } else require(((Seed<String>) seed).read().equals("owned value") && reads == 1);
            }
            System.out.println("route=" + route + ",operation=" + operation + ",mode=" + mode
                + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
