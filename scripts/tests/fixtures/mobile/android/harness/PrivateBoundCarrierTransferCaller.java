import fixture.*;

public final class PrivateBoundCarrierTransferCaller {
    public static Object call(Core subject, Slot<?> input, int shape) {
        if (shape == 0) return Core.call(subject, input);
        if (shape == 1) return Core.object(subject, input);
        if (shape == 2) { Core.discard(subject, input); return null; }
        return Core.checked(subject, input);
    }
}
