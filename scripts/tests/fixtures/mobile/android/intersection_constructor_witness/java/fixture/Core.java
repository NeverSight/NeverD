package fixture;
public class Core extends Value {
    public Product result;
    public Core() { Trace.hit(1); result = new Product(this); Trace.hit(8); }
    public static Product make(Value value) {
        Trace.hit(1);
        Product product = new Product(value);
        Trace.hit(8);
        return product;
    }
    public static Product nested(Value value) {
        Trace.hit(1);
        return Trace.finish(new Product(value));
    }
}
