import fixture.*;
public final class MixedCarrierCaller {
    public static Object call(Object[] inputs, Saver<?,?> saver, Factory<?> producer, long flags) {
        return Client.call(inputs, saver, producer, flags);
    }
}
