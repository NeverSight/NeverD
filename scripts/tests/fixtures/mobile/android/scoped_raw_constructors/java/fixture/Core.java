package fixture;
public class Core<K, V> extends Parent<K, V> {
    public Object local;
    static {
        Trace.hit(9);
        ((Parent) Trace.source).key = Trace.initialKey;
        ((Parent) Trace.source).value = Trace.initialValue;
    }
    public Core(Parent input) { super(input); }
}
