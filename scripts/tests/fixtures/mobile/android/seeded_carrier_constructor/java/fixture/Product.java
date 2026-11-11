package fixture;
public final class Product extends Parent<Product> {
    static {
        Trace.hit(2);
        Trace.initAction.run();
    }
    public <T> Product(T seed, Token<T> token) {
        super(seed, token);
        Trace.kind = 2;
        Trace.hit(9);
    }
    public <T> Product(T seed, Token<T> token, float weight) {
        super(seed, token);
        Trace.weight = weight;
        Trace.kind = 3;
        Trace.hit(9);
    }
    public Product(Value value) {
        super(value);
        Trace.kind = 1;
    }
}
