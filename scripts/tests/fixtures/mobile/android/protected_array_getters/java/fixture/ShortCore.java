package fixture;
public final class ShortCore extends Base<short[]> {
    public int calls, length;
    public boolean fresh;
    public short[] value, last;
    public RuntimeException failure;
    protected short[] get() {
        calls++;
        if (failure != null) throw failure;
        if (fresh) value = new short[length];
        last = value;
        return value;
    }
}
