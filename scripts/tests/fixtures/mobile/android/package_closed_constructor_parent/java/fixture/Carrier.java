package fixture;

public class Carrier<K, V> {
    public int value;
    public Carrier(int value) {
        this.value = value;
        Trace.hit(value, null, this, value);
    }
}
