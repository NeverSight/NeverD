package fixture;
public class Carrier<T extends Base> {
    public T value;
    public Probe probe;
    public RuntimeException failure;
    public Carrier(T value, Probe probe, RuntimeException failure) {
        this.value = value;
        this.probe = probe;
        this.failure = failure;
    }
}
