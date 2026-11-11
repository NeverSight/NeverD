package fixture;
public class Owner {
    static { Trace.step(1); }
    public int tag;
    public Sink<String> sink;
    public Owner(int value) {
        this(value, null);
        Trace.step(4);
    }
    public Owner(int value, Sink<String> target) {
        Trace.step(2);
        tag = value;
        sink = target;
        Trace.step(3);
    }
    public Owner(int value, Object target) {
        Trace.step(9);
        tag = -value;
    }
}
