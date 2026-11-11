package fixture;
public class Receiver extends Parent<Leaf> {
    public Leaf value;
    public Receiver() { super(); }
    public Leaf create() {
        Trace.events = Trace.events * 10 + 1;
        if (Trace.targetFailure != null) throw Trace.targetFailure;
        return value;
    }
    public static Base checked(Object value) { return (Base) value; }
}
