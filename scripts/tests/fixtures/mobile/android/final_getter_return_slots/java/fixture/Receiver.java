package fixture;

public class Receiver<A,B> extends Parent<A> {
    public Leaf<A> value;
    public Receiver() {}
    public Leaf<A> getItems() {
        Trace.events = Trace.events * 10 + 1;
        if (Trace.failure != null) throw Trace.failure;
        return value;
    }
    public final Leaf<A> specificItems() { return getItems(); }
    public static Leaf<?> select(Receiver<?,?> receiver) { return receiver.specificItems(); }
}
