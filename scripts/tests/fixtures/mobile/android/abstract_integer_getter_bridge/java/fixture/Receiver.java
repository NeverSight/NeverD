package fixture;
public class Receiver<S,U> extends Parent<S> implements CountView<S> {
    public int value, calls;
    public Receiver() {}
    public int getCount() {
        Trace.events = Trace.events * 10 + 2;
        calls++;
        if (Trace.failure != null) throw Trace.failure;
        return value;
    }
}
