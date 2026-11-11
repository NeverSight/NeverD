package fixture;

public interface Getter extends Value<Double> {
    double read();
    default Double getValue() { Trace.mark(1); return Double.valueOf(read()); }
}
