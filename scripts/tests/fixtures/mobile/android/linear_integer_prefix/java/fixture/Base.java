package fixture;
public class Base {
    static { Trace.hit(2); }
    public final int argument;
    public Base(int value) {
        argument = value; Trace.base = this; Trace.hit(3);
        int seen = observe(); Trace.observed = seen;
        Trace.input.flag = !Trace.input.flag;
    }
    protected int observe() { return -1; }
}
