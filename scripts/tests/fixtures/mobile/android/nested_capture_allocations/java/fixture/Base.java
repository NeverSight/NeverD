package fixture;
public abstract class Base<T> implements Reader<T> {
    static { Trace.step(2); }
    public Base(int event) { Trace.step(event); }
}
