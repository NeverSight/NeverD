import fixture.*;
public final class SeedFactoryCaller {
    public static Box<?> seed(Object value, long flags) { return Client.seed(value, flags); }
}
