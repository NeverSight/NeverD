package fixture;
public final class Checked {
    public final Object value;
    public Checked(Object value) { this.value = value; }
    public Object read() {
        Trace.seen = this;
        Provider.current = Provider.late;
        Trace.step(3);
        return value;
    }
}
