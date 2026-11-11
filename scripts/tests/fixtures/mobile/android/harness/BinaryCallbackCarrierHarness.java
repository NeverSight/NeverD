import fixture.Carrier;
import fixture.Client;
import fixture.Comparison;
import fixture.Provider;
import fixture.Trace;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class BinaryCallbackCarrierHarness {
    static void require(boolean value, String message) {
        if (!value) throw new AssertionError("owned binary callback carrier: " + message);
    }
    static Carrier call(Carrier source, Comparison comparison, int route) throws Throwable {
        if (route == 0) return Client.call(source, comparison);
        if (route == 1) return BinaryCallbackCarrierCaller.call(source, comparison);
        if (route == 2) {
            try { return (Carrier)Client.class.getMethod("call", Carrier.class, Comparison.class).invoke(null, source, comparison); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (Carrier)MethodHandles.publicLookup().findStatic(Client.class, "call",
                MethodType.methodType(Carrier.class, Carrier.class, Comparison.class)).invoke(source, comparison);
    }
    static long trace(int... stages) {
        long value = 0;
        for (int stage : stages) value = value * 10 + stage;
        return value;
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        boolean invariant = Boolean.parseBoolean(args[2]);
        String expectedSignature = "public static <T> fixture.Carrier<T> fixture.Provider.select(fixture.Carrier<" +
                (invariant ? "T" : "? extends T") + ">,fixture.Comparison<? super T, ? super T, java.lang.Boolean>)";
        require(Provider.class.getMethod("select", Carrier.class, Comparison.class).toGenericString().equals(expectedSignature),
                "complete method signature changed");
        Trace.failure = new RuntimeException("owned provider failure");
        Error fatal = new Error("owned callback failure");
        Trace.initFatal = mode == 22 ? fatal : null;
        Trace.failAt = mode == 3 ? 2 : mode == 4 ? 4 : mode == 5 ? 1 : mode == 6 ? 3 :
                mode == 7 ? 5 : mode == 8 ? 6 : mode == 9 ? 7 : mode == 10 ? 8 :
                mode == 20 ? 10 : mode == 21 ? 9 : 0;
        int[] left = {17}, right = mode == 15 ? left : new int[]{19};
        Object first = mode == 13 ? null : left;
        Object second = mode == 14 ? null : right;
        Object sentinel = new Object();
        int[] reads = {0}, calls = {0}, outputReads = {0};
        Carrier<Object> source = () -> {
            int index = reads[0]++;
            require(index < 2, "unexpected source traversal");
            Trace.step(index == 0 ? 2 : 4);
            if (index == 1) require(Trace.first == first, "first publication is late");
            return index == 0 ? first : second;
        };
        Boolean accepted = mode == 11 ? null : mode == 12 ? Boolean.FALSE : Boolean.TRUE;
        Carrier<Object> output = () -> {
            outputReads[0]++;
            Trace.step(10);
            return mode == 19 ? new Object() : "owned output";
        };
        Carrier<Object> alternate = () -> { throw new AssertionError("provider predicted an opaque result"); };
        Comparison<Object,Object,Boolean> callback = (a, b) -> {
            Trace.step(6);
            require(a == first && b == second && Trace.first == first && Trace.second == second && reads[0] == 2,
                    "callback arguments or publication changed");
            calls[0]++;
            if (a != null) ((int[])a)[0]++;
            if (b != null) ((int[])b)[0] += 2;
            if (mode == 17) Trace.output = alternate;
            if (mode == 23) throw fatal;
            return accepted;
        };
        Comparison<String,String,Boolean> wrong = (a, b) -> { throw new AssertionError("missing callback cast"); };
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.trace = 0; Trace.first = sentinel; Trace.second = sentinel;
            Trace.callbackResult = sentinel; Trace.flag = 2;
            Trace.output = mode == 18 ? null : output;
            reads[0] = 0; outputReads[0] = 0;
            int oldCalls = calls[0], oldLeft = left[0], oldRight = right[0];
            Carrier result = null;
            Throwable error = null;
            try {
                result = call(mode == 1 ? null : source, mode == 2 ? null : mode == 16 ? wrong : callback, route);
                if (mode == 19 || mode == 20) BinaryCallbackCarrierCaller.take((Carrier<String>)result);
            } catch (Throwable failure) { error = failure; }
            int[] stages = mode == 21 || mode == 22 ? new int[]{} :
                    mode == 1 || mode == 5 ? new int[]{1} : mode == 3 ? new int[]{1,2} :
                    mode == 6 ? new int[]{1,2,3} : mode == 4 ? new int[]{1,2,3,4} :
                    mode == 2 || mode == 7 || mode == 16 ? new int[]{1,2,3,4,5} :
                    mode == 8 || mode == 23 ? new int[]{1,2,3,4,5,6} :
                    mode == 9 || mode == 11 ? new int[]{1,2,3,4,5,6,7} :
                    mode == 19 || mode == 20 ? new int[]{1,2,3,4,5,6,7,8,10} :
                    new int[]{1,2,3,4,5,6,7,8};
            long expected = trace(stages);
            if (attempt == 0) {
                long place = 1;
                for (int stage : stages) place *= 10;
                expected += 9 * place;
            }
            require(Trace.trace == expected, "provider/callback/initialization order changed");
            boolean initialized = mode != 21 && mode != 22;
            boolean gotFirst = initialized && mode != 1 && mode != 3 && mode != 5;
            boolean gotSecond = gotFirst && mode != 4 && mode != 6;
            require(Trace.first == (gotFirst ? first : sentinel) && Trace.second == (gotSecond ? second : sentinel),
                    "original partial publication changed");
            int expectedReads = !initialized || mode == 1 || mode == 5 ? 0 : mode == 3 || mode == 6 ? 1 : 2;
            require(reads[0] == expectedReads, "original source call count changed");
            boolean callbackCompleted = gotSecond && mode != 2 && mode != 7 && mode != 8 && mode != 16;
            require(calls[0] == oldCalls + (callbackCompleted ? 1 : 0), "original comparison call count changed");
            require(left[0] == oldLeft + (callbackCompleted ? (first == null ? 0 : 1) + (second == left ? 2 : 0) : 0),
                    "left mutable alias changed");
            if (right != left) require(right[0] == oldRight + (callbackCompleted && second != null ? 2 : 0),
                    "right mutable alias changed");
            boolean returnedCallback = callbackCompleted && mode != 23;
            require(Trace.callbackResult == (returnedCallback ? accepted : sentinel), "original callback result changed");
            boolean unboxed = returnedCallback && mode != 9 && mode != 11;
            require(Trace.flag == (unboxed ? (mode == 12 ? 0 : 1) : 2), "original unboxing order changed");
            if (mode == 21) {
                if (attempt == 0) require(error instanceof ExceptionInInitializerError && error.getCause() == Trace.failure,
                        "original initializer cause changed");
                else require(error instanceof NoClassDefFoundError, "initializer failure is not sticky");
            } else if (mode == 22) {
                require(attempt == 0 ? error == fatal : error instanceof NoClassDefFoundError, "original initializer Error changed");
            } else if (mode == 23) require(error == fatal, "original callback Error changed");
            else if (mode == 1 || mode == 2 || mode == 11) require(error instanceof NullPointerException, "original null failure changed");
            else if (mode == 16 || mode == 19) require(error instanceof ClassCastException, "original checked cast changed");
            else if (mode >= 3 && mode <= 10 || mode == 20) require(error == Trace.failure, "original exception identity changed");
            else require(error == null, "unexpected original failure");
            boolean returned = initialized && !(mode >= 1 && mode <= 11) && mode != 16 && mode != 23;
            if (returned) require(result == Trace.output && result == (mode == 17 ? alternate : mode == 18 ? null : output),
                    "opaque returned carrier changed");
            require(outputReads[0] == (mode == 19 || mode == 20 ? 1 : 0), "provider speculated a returned value");
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.trace);
        }
    }
}
