import fixture.*;
public final class PrivateSeededCarrierInputCaller {
    public static Carrier call(Carrier input, Object seed, boolean add, int shape) {
        if (shape == 0) return Core.call(input, seed, add);
        Core.discard(input, seed, add); return null;
    }
}
