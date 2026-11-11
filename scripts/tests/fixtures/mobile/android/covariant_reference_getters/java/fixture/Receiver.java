package fixture;
public class Receiver<S, V> extends Parent<S> {
    public Leaf<S> value;
    public Receiver() {}
    public Leaf<S> create() {
        Trace.events = Trace.events * 10 + 1;
        if (Trace.targetFailure != null) throw Trace.targetFailure;
        return value;
    }
    public static Base<?> checked(Object value) { return (Base<?>) value; }
}
