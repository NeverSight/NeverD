package fixture;
public final class Core {
    public static Product make(Object seed, Token<Object> token) {
        Trace.hit(1);
        Product product = new Product(seed, token);
        Trace.hit(8);
        return product;
    }
    public static Product weighted(Object seed, Token<Object> token, float weight) {
        Trace.hit(1);
        Product product = new Product(seed, token, weight);
        Trace.hit(8);
        return product;
    }
    public static Product nested(Object seed, Token<Object> token) {
        Trace.hit(1);
        return Trace.finish(new Product(seed, token));
    }
}
