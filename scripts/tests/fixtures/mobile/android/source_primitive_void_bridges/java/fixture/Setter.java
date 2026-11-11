package fixture;
@SuppressWarnings("rawtypes")
public interface Setter extends Value, Marker<String> {
    double read();
    void accept(double value);
    default Double get() { return Double.valueOf(read()); }
    default void set(double value) { Trace.mark(1); accept(value); }
    default void set(Object value) { set(((Number)value).doubleValue()); }
}
