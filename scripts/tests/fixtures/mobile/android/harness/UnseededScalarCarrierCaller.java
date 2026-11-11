import fixture.*;
public class UnseededScalarCarrierCaller {
    public static boolean call(int entry, Carrier<?> values, Object input) {
        if (entry == 0) return Core.call(values, input);
        if (entry == 1) return Core.guarded(values, input);
        return Core.locked(values, input);
    }
}
