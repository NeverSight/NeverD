package fixture;
public class Receiver<S, V> extends Upper<S, V> {
    public Leaf<V> value;
    public Receiver() {}
    public Leaf<V> create() {
        Trace.events = Trace.events * 10 + 1;
        if (Trace.targetFailure != null) throw Trace.targetFailure;
        return value;
    }
    public static Base<?> checked(Object value) { return (Base<?>) value; }
}
