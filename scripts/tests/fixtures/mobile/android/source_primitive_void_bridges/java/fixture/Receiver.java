package fixture;
@SuppressWarnings("rawtypes")
public class Receiver<S> implements Setter, Mirror {
    public double value;
    public int calls;
    public Receiver() {}
    public double read() { return value; }
    public void accept(double next) {
        Trace.mark(2);
        calls++;
        if (Trace.failure != null) throw Trace.failure;
        value = next;
    }
}
