package fixture;
public class Typed implements Value<Double> {
    public Double value;
    public Typed() {}
    public Double get() { return value; }
    public void set(Double next) { Trace.mark(4); value = next; }
}
