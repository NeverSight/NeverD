import fixture.*;
public final class SeededNominalResultCaller {
    public static FixedResult call(Object seed, Carrier<?, ?> callback, float[] values) {
        return Client.call(seed, callback, values);
    }
}
