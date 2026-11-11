package fixture;
public class Carrier<T> {
    public final Object value;
    public int reads;
    public Carrier(Object value) { this.value=value; }
    @SuppressWarnings("unchecked") public T read() { reads++;Trace.hit(5);return (T)value; }
}
