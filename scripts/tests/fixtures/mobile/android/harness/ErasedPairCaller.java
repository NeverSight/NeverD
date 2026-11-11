import fixture.*;
public final class ErasedPairCaller {
    public static Box call(Root receiver, Transform transform, Callback callback, long flags) {
        return Client.call(receiver, transform, callback, flags);
    }
    public static Box iface(API receiver, Transform transform, Callback callback, long flags) {
        return Client.iface(receiver, transform, callback, flags);
    }
}
