package fixture;
public final class Token {
    static { Trace.hit(9); }
    public final Object value;
    public Token(Object value) { Trace.token = this; Trace.hit(14); this.value = value; }
}
