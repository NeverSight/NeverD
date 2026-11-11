package fixture;
@SuppressWarnings("unchecked")
public class Provider {
    static { Trace.hit(1); }
    public int calls;
    public Object value;
    public <K, V> Pair<K, V> make() {
        Trace.hit(2);
        calls++; Trace.seen = this;
        Pair<K, V> result = (Pair<K, V>)value;
        Trace.hit(3);
        return result;
    }
}
