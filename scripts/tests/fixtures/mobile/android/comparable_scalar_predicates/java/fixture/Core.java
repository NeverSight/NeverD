package fixture;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class Core extends Parent<Object> implements Api, OtherApi {
    public Character value;
    public boolean contains(char value) { return super.scalar(value); }
    public boolean contains(Comparable value) { return contains(((Character) value).charValue()); }
    public Character get() { return value; }
    public boolean isEmpty() { return value == null; }
}
