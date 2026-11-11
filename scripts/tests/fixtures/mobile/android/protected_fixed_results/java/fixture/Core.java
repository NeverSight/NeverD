package fixture;
public abstract class Core<E> extends Base<Slot> {
    public int calls, resolves, index;
    public long wide, bits;
    public Slot last;
    public RuntimeException failure;
    protected Slot resolve(Token token) { resolves++; last = token.value; return last; }
    protected final Slot get(Token token, int index, long wide, double fraction) {
        calls++; this.index = index; this.wide = wide;
        bits = Double.doubleToRawLongBits(fraction);
        token.reads++; token.receiver = this;
        if (failure != null) throw failure;
        return resolve(token);
    }
    public Slot typed(Token token, int index, long wide, double fraction) { return get(token, index, wide, fraction); }
}
