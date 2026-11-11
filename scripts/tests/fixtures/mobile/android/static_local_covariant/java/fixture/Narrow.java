package fixture;
public final class Narrow implements Item {
    public final Object value;
    public Narrow(Object value) { this.value=value; }
    public Object token() { return value; }
}
