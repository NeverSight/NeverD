package fixture;
@SuppressWarnings("rawtypes")
public final class Core implements Api, Marker<Carrier> {
    public int calls;
    public long value;
    public RuntimeException failure;
    public Token token;
    public void write(Token next, Object value) { writeValue(next, ((Carrier)value).readBits()); }
    public void writeValue(Token next, long value) {
        calls++; this.value = value;
        next.writes++; next.receiver = this;
        if (failure != null) throw failure;
        next.accepted = value;
    }
    public int reads;
    public RuntimeException readFailure;
    public Object read(Token next) { return Carrier.box(readValue(next)); }
    public long readValue(Token next) {
        reads++; next.reads++; next.receiver = this;
        if (readFailure != null) throw readFailure;
        return next.bits;
    }
    public Token meta() { return token; }
}
