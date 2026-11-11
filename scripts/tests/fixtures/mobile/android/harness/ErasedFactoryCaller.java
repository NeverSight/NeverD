import fixture.*;
public final class ErasedFactoryCaller {
    public static Box<?> call(Factory<?> factory, long flags) { return Client.call(factory, flags); }
}
