import fixture.*;
public final class DelegationContinuationCaller {
    public static Core create(int variant, Object token, boolean flag, Holder holder) {
        if (variant == 0) return new Core(token, flag);
        if (variant == 1) return new Core(token, flag, holder);
        if (variant == 2) return new Core(token, flag, holder, (byte) 0);
        if (variant == 3) return new Core(token, flag, holder, (char) 0);
        if (variant == 4) return new Core(token, flag, holder, (short) 0);
        if (variant == 5) return new Core(token, flag, holder, 0);
        return new Core(token, flag, holder, 0L);
    }
}
