package fixture;
public final class IntegerCore extends Base<int[]> {
    public int calls, length;
    public boolean fresh;
    public int[] value, last;
    public RuntimeException failure;
    protected int[] get() {
        calls++;
        if (failure != null) throw failure;
        if (fresh) value = new int[length];
        last = value;
        return value;
    }
}
