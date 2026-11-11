import fixture.*;
public final class IndependentCarrierFactoryCaller {
    public static Object call(int operation, Seed seed, int code, Context first, Context second) {
        return operation == 0 ? Client.call(seed, code, first, second) : Client.checked(seed, code, first, second);
    }
}
