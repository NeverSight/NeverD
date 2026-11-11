package fixture;
public class Core<K, V> extends Parent<K, V> {
    public Core(int value) { super(value); Trace.hit(5); }
    public Core(long value) { super(value); Trace.hit(5); }
}
