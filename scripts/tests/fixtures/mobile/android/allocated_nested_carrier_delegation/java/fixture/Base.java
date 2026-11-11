package fixture;

public class Base<K, V> {
    public Base() {
        Trace.parents++;
        Trace.partial = this;
        Trace.stage = 3;
    }
}
