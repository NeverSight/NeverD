import fixture.*;
public final class PairedVoidCaller {
    public static void call(Provider target, int flags, Pair pair, Object input, Object token, long bits) {
        Client.call(target, flags, pair, input, token, bits);
    }
}
