package fixture;
public abstract class Base<T> {
    public Base() {}
    public abstract void set$contract(T value);
    public void through(T value) {
        Trace.events = Trace.events * 10 + 3;
        set$contract(value);
    }
}
