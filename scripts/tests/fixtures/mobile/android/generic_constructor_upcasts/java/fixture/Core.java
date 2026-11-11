package fixture;
final class Core<K,V> extends Parent<K,V> {
    static { Trace.hit(2); }
    public Derived<K,V> original;
    @SuppressWarnings("unchecked")
    public Core(Derived<K,V> value) {
        Trace.before(value, "owned constructor upcast");
        super((Base<K,V>)(Object)value);
        original = value;
        Trace.hit(5);
    }
}
