package fixture;
public final class Token<T> {
    public final Object value;
    public Token(Object value) { this.value = value; }
    @SuppressWarnings("unchecked")
    public T read() { Trace.hit(4, this, value, null); return (T)value; }
}
