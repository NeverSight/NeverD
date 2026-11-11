package fixture;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class Core extends Parent<Object> implements Api, OtherApi {
    public Double value;
    public boolean contains(double value) { return super.scalar(value); }
    public boolean contains(Comparable value) { return contains(((Number) value).doubleValue()); }
    public Double get() { return value; }
    public boolean isEmpty() { return value == null; }
    public boolean before(double left, double right) { return super.pair(left, right); }
    public boolean before(Comparable left, Comparable right) {
        return before(((Number) left).doubleValue(), ((Number) right).doubleValue());
    }
}
