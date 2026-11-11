package fixture;
public abstract class Api<T> {
    public int containsCalls, indexCalls;
    public Object seen;
    public boolean contains(Object value) { containsCalls++; seen = value; return observe(value) == value; }
    public int index(Object value) { indexCalls++; seen = value; return observe(value) == value ? 7 : 8; }
    public abstract Object observe(Object value);
}
