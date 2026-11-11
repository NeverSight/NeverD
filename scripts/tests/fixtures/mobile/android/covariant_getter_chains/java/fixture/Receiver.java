package fixture;
public class Receiver<T,U> implements View<T> {
    public Leaf<T> value;
    public int calls;
    public Receiver() {}
    public Leaf<T> getCreated() {
        Trace.events = Trace.events * 10 + 1;
        calls++;
        if (Trace.failure != null) throw Trace.failure;
        return value;
    }
    public final Leaf<T> create() { return getCreated(); }
    public static Base<?> checked(Object value) { return (Base<?>)value; }
}
