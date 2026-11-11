package fixture;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class Core extends Parent<Object> implements Api, OtherApi {
    public long value;
    public int reads;
    public RuntimeException readFailure;
    public boolean containsScalar(long value) { return super.scalar(value); }
    public boolean contains(Comparable value) { return containsScalar(((Carrier) value).read()); }
    public long readScalar() {
        reads++;
        if (readFailure != null) throw readFailure;
        return value;
    }
    public Comparable get() { return Carrier.make(readScalar()); }
    public boolean isEmpty() { return value == 0; }
    public boolean beforeScalar(long left, long right) { return super.pair(left, right); }
    public boolean before(Comparable left, Comparable right) {
        return beforeScalar(((Carrier) left).read(), ((Carrier) right).read());
    }
}
