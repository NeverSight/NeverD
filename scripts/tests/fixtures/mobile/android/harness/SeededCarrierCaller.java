import fixture.*;
public final class SeededCarrierCaller {
    public static Box<?> call(Object seed, Fn<?,?> callback, long flags) {
        return Client.call(seed, callback, flags);
    }
}
