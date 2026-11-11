import fixture.*;
public final class PrivateInstanceCarrierCaller {
    public static void call(Provider receiver, boolean present, int flags, Object input, Producer producer, long bits) {
        receiver.call(present, flags, input, producer, bits);
    }
}
