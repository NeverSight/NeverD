package fixture;

public class Receiver<S> implements Getter, Mirror<Double> {
    public double value;
    public RuntimeException failure;
    public int calls;
    public Receiver() {}
    public double read() {
        calls++; Trace.mark(2);
        if (failure != null) throw failure;
        return value;
    }
}
