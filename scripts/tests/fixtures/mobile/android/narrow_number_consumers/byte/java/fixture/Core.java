package fixture;
@SuppressWarnings("rawtypes")
public final class Core implements Api, Marker<Byte> {
    public int calls;
    public byte value;
    public RuntimeException failure;
    public Token token;
    public void write(Token next, Object value) { write(next, ((Number)value).byteValue()); }
    public void write(Token next, byte value) {
        calls++; this.value = value;
        next.writes++; next.receiver = this;
        if (failure != null) throw failure;
        next.accepted = value;
    }
    public Byte read(Token next) { return next.saved; }
    public Token meta() { return token; }
}
