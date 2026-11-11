package fixture;
public final class Checked {
    public final Object data;
    public Checked(Object data) { this.data=data; }
    public Object value() { Trace.event(4); return data; }
}
