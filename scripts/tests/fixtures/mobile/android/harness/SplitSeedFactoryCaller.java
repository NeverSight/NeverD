import fixture.*;
public final class SplitSeedFactoryCaller {
    public static Carrier call(Carrier parent, Object first, Object second, String label, int mode) {
        return Provider.create(parent, first, second, label, mode);
    }
}
