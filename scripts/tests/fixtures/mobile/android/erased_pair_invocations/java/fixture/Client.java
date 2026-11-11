package fixture;
public final class Client {
    public static Box call(Root receiver, Transform transform, Callback callback, long flags) {
        Trace.flags = flags;
        return receiver.make(transform, callback);
    }
    public static Box iface(API receiver, Transform transform, Callback callback, long flags) {
        Trace.flags = flags;
        return receiver.make(transform, callback, flags);
    }
}
