package fixture;
public final class LongCore extends Base<long[]> {
    public int calls, index;
    public Object seen;
    public RuntimeException failure;
    protected void write(Token token, long[] array, int index) {
        calls++; seen = array; this.index = index;
        if (failure != null) throw failure;
        token.calls++;
        array[index] = 0x1020304050607080L;
    }
}
