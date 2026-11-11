import fixture.*;
public final class SeededCarrierConstructorCaller {
    public static <T> Product call(T seed, Token<T> token) {
        Trace.hit(1);
        Product product = new Product(seed, token);
        Trace.hit(8);
        return product;
    }
}
