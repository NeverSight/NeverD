package fixture;
@SuppressWarnings("rawtypes")
public final class Core implements Api, Marker<Carrier> {
    public int calls;
    public int value;
    public RuntimeException failure;
    public Token token;
    public void write(Token next, Object value) { writeValue(next, ((Carrier)value).readBits()); }
    public void writeValue(Token next, int value) {
        calls++; this.value = value;
        next.writes++; next.receiver = this;
        if (failure != null) throw failure;
        next.accepted = value;
    }
    public Carrier read(Token next) { return next.saved; }
    public Token meta() { return token; }
}
