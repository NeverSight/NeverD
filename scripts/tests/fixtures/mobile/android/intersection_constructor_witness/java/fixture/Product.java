package fixture;
public final class Product {
    static { Trace.hit(2); Trace.initAction.run(); }
    public <T extends Bound & Flag> Product(T value) { this(value, value); }
    public Product(Flag flag, Bound value) {
        Trace.hit(3);
        Trace.product = this;
        Trace.flag = flag;
        Trace.value = value;
        Trace.observed = value.number;
        Trace.hit(4);
        flag.touch();
        Trace.hit(6);
        Trace.action.run();
        Trace.hit(7);
    }
}
