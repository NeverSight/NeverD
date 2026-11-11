package fixture;
public class Carrier<E extends Bound> {
    public E value;
    public Probe probe;
    public RuntimeException failure;
}
