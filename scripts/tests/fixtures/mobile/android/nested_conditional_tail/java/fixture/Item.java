package fixture;
public final class Item {
    static { Trace.step(2); Trace.provider.payload = Trace.innerPayload; }
    public final Object value;
    public Item(Object input) {
        Trace.item = this;
        Trace.step(4);
        value = input;
        Trace.step(5);
    }
}
