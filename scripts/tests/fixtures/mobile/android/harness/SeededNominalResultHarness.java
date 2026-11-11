import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class SeededNominalResultHarness {
    static void require(boolean condition) {
        if (!condition) throw new AssertionError("seeded nominal result contract");
    }
    static FixedResult call(Object seed, Carrier<?, ?> callback, float[] values, int route) throws Throwable {
        if (route == 0) return Client.call(seed, callback, values);
        if (route == 1) return SeededNominalResultCaller.call(seed, callback, values);
        if (route == 2) {
            try { return (FixedResult)Client.class.getMethod("call", Object.class, Carrier.class, float[].class)
                    .invoke(null, seed, callback, values); }
            catch (InvocationTargetException error) { throw error.getCause(); }
        }
        return (FixedResult)MethodHandles.lookup().findStatic(Client.class, "call",
                MethodType.methodType(FixedResult.class, Object.class, Carrier.class, float[].class))
                .invokeExact(seed, (Carrier)callback, values);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        require(Provider.class.getMethod("make", Object.class, Carrier.class, float[].class).isVarArgs());
        require(!Client.class.getMethod("call", Object.class, Carrier.class, float[].class).isVarArgs());
        require(FixedResult.class.getTypeParameters().length == 0);
        require(FixedResult.class.getGenericSuperclass().getTypeName().equals("fixture.ResultBase<java.lang.String>"));
        Trace.failure = new RuntimeException("owned seeded nominal failure");
        Trace.failAt = mode >= 1 && mode <= 4 ? mode : 0;
        int[] value = {0};
        Object seed = mode == 6 ? "incompatible seed" : mode == 7 ? null : value;
        Object token = new Object();
        FixedResult fixed = new FixedResult(token);
        Trace.result = mode == 10 ? null : fixed;
        Error callbackFailure = new AssertionError("owned frozen callback failure");
        SeededNominalResultCallback callback = new SeededNominalResultCallback();
        callback.expected = seed;
        callback.answer = mode == 9 ? null : Float.valueOf(Float.intBitsToFloat(0x7fc00789));
        callback.failure = mode == 11 ? callbackFailure : null;
        callback.mutate = mode == 14;
        SeededNominalResultRawCallback raw = new SeededNominalResultRawCallback();
        raw.expected = seed;
        Float untouched = Float.valueOf(17.0f);
        float[] untouchedArray = {31.0f};
        for (int attempt = 0; attempt < 2; attempt++) {
            int firstBits = attempt == 0 ? 0x80000000 : 0x7fc00123;
            float[] values = mode == 8 ? null : mode == 13 ? new float[0] :
                    new float[]{Float.intBitsToFloat(firstBits), Float.intBitsToFloat(0xffc00abc)};
            callback.values = values; raw.values = values;
            Trace.events = 0; Trace.callbackResult = untouched; Trace.values = untouchedArray;
            int oldCalls = callback.calls, oldRawCalls = raw.calls, oldValue = value[0];
            Throwable failure = null; FixedResult result = null;
            try { result = call(seed, mode == 5 ? null : mode == 12 ? raw : callback, values, route); }
            catch (Throwable error) { failure = error; }
            String expected = mode == 1 ? "" : mode == 2 || mode == 5 || mode == 6 ? "2" :
                    mode == 3 || mode == 11 || mode == 12 ? "23" : "234";
            if (attempt == 0) expected = "1" + expected;
            require(Trace.events == (expected.isEmpty() ? 0 : Long.parseLong(expected)));
            if (mode == 1) {
                if (attempt == 0) require(failure instanceof ExceptionInInitializerError && failure.getCause() == Trace.failure);
                else require(failure instanceof NoClassDefFoundError);
            } else if (mode >= 2 && mode <= 4) require(failure == Trace.failure);
            else if (mode == 5) require(failure instanceof NullPointerException);
            else if (mode == 6 || mode == 12) require(failure instanceof ClassCastException);
            else if (mode == 11) require(failure == callbackFailure);
            else require(failure == null && result == Trace.result && (result == null || result.token == token));
            boolean entered = mode != 1 && mode != 2 && mode != 5 && mode != 6;
            boolean completed = entered && mode != 3 && mode != 11 && mode != 12;
            require(callback.calls == oldCalls + (entered && mode != 12 ? 1 : 0));
            require(raw.calls == oldRawCalls + (mode == 12 ? 1 : 0));
            require(value[0] == oldValue + (completed && mode != 7 ? 1 : 0));
            require(Trace.callbackResult == (completed ? callback.answer : untouched));
            require(Trace.values == (mode == 1 ? untouchedArray : values));
            if (values != null && values.length != 0) {
                require(Float.floatToRawIntBits(values[0]) == (mode == 14 ? 0x7fc00456 : firstBits));
                require(Float.floatToRawIntBits(values[1]) == 0xffc00abc);
            }
            if (completed && callback.answer != null)
                require(Float.floatToRawIntBits(Trace.callbackResult.floatValue()) == 0x7fc00789);
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
