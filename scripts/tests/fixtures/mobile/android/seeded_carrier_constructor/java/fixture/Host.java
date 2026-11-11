package fixture;
public final class Host<S extends Value> {
    public static Token<Host<?>> current;
    public S value;
    public Product product;
    public Host(S value) {
        this.value = value;
        Trace.hit(1);
        product = new Product(this, current);
        Trace.hit(8);
    }
}
