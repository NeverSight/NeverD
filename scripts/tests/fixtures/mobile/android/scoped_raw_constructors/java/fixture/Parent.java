package fixture;
public class Parent<K, V> {
    public Object key, value;
    public Parent(int ignored) {}
    public Parent(Parent<? extends K, ? extends V> input) {
        Trace.hit(1);
        Trace.published = this;
        key = input.key;
        Trace.hit(2);
        value = input.value;
        Trace.hit(3);
    }
}
