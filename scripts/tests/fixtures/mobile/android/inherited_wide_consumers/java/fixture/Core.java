package fixture;
@SuppressWarnings("rawtypes")
public final class Core implements Api, Marker<Long> {
    public int calls;
    public long value;
    public RuntimeException failure;
    public Token token;
    public void write(Token next, Object value) { write(next, ((Number)value).longValue()); }
    public void write(Token next, long value) {
        calls++; this.value = value;
        next.writes++; next.receiver = this;
        if (failure != null) throw failure;
        next.accepted = value;
    }
    public Long read(Token next) { return next.saved; }
    public Token meta() { return token; }
}
