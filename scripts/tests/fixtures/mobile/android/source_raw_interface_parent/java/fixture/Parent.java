package fixture;
public abstract class Parent<P> implements Leaf, View {
    public Box<Object> stored;
    public final Box<Object> elements() {
        Trace.events = Trace.events * 10 + 2;
        if (Trace.firstFailure != null) throw Trace.firstFailure;
        return stored;
    }
    public abstract P make();
}
