package fixture;
public class Core {
    public final String value;
    public Box<Value> stored;
    Core(Provider provider) { this(provider, Factory.<Value>empty()); }
    Core(Provider provider, Box<Value> values) {
        this(provider.id());
        Trace.hit(2, this, provider, values);
        this.stored = values;
        Trace.hit(4, this, provider, values);
    }
    public Core(String id) {
        Trace.hit(1, this, id, null);
        this.value = id;
        Trace.hit(3, this, id, null);
    }
    public String kind() { return "base"; }
}
