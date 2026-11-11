package fixture;
public class Provider {
    static { Trace.hit(1); }
    public static <T> FixedResult make(T seed, Carrier<T, Float> callback, float... values) {
        Trace.values = values; Trace.hit(2);
        Float result = callback.apply(seed);
        Trace.callbackResult = result; Trace.hit(4);
        return Trace.result;
    }
}
