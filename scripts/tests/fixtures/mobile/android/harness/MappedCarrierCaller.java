import fixture.*;
public final class MappedCarrierCaller {
    public static Box<?> call(Box<?> seed, Fn<?,?> callback, long flags) {
        return Client.call(seed, callback, flags);
    }
}
