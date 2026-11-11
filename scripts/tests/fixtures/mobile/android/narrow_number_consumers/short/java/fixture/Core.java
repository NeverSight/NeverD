package fixture;
@SuppressWarnings("rawtypes")
public final class Core implements Api, Marker<Short> {
    public int calls;
    public short value;
    public RuntimeException failure;
    public Token token;
    public void write(Token next, Object value) { write(next, ((Number)value).shortValue()); }
    public void write(Token next, short value) {
        calls++; this.value = value;
        next.writes++; next.receiver = this;
        if (failure != null) throw failure;
        next.accepted = value;
    }
    public Short read(Token next) { return next.saved; }
    public Token meta() { return token; }
}
