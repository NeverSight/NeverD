import fixture.*;
public final class NestedCarrierCaller {
    public static Box<?> call(Fn<?,?,?> callback, long flags) { return Client.call(callback, flags); }
}
