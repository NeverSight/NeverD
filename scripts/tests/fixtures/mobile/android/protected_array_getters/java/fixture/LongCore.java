package fixture;
public final class LongCore extends Base<long[]> {
    public int calls, length;
    public boolean fresh;
    public long[] value, last;
    public RuntimeException failure;
    protected long[] get() {
        calls++;
        if (failure != null) throw failure;
        if (fresh) value = new long[length];
        last = value;
        return value;
    }
}
