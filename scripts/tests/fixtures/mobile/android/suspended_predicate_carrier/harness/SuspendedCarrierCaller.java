import fixture.*;
public final class SuspendedCarrierCaller {
    public static Carrier pick(Carrier source, Callback callback) { return Client.pick(source, callback); }
}
