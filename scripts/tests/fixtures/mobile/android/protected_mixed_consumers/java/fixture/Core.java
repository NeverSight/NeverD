package fixture;
public final class Core extends Middle<Special> {
    public int calls, index;
    public boolean flag;
    public long cookie;
    public double weight;
    public Token tokenSeen;
    public Object seen;
    public RuntimeException failure;
    protected void read(Token token, int index, Special builder, boolean flag, long cookie, double weight) {
        calls++; tokenSeen = token; seen = builder; this.index = index; this.flag = flag;
        this.cookie = cookie; this.weight = weight;
        if (failure != null) throw failure;
        token.calls++;
        builder.calls++;
    }
}
