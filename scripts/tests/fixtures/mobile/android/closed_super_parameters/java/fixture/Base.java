package fixture;
public class Base<T> {
    public T last;
    public Probe probe;
    public Object marker;
    public RuntimeException failure;
    public void deliver(T value) {
        Core.events = Core.events * 5 + 1;
        last = value;
        marker = probe.observe(value);
        if (failure != null) throw failure;
        Core.events = Core.events * 5 + 2;
    }
}
