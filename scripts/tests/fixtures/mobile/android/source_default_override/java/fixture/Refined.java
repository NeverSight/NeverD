package fixture;

public interface Refined extends Getter, Mirror<Double> {
    default Double getValue() { Trace.mark(4); return Double.valueOf(read()); }
}
