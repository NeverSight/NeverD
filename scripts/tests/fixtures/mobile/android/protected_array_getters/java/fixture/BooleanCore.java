package fixture;
public final class BooleanCore extends Base<boolean[]> {
    public int calls, length;
    public boolean fresh;
    public boolean[] value, last;
    public RuntimeException failure;
    protected boolean[] get() {
        calls++;
        if (failure != null) throw failure;
        if (fresh) value = new boolean[length];
        last = value;
        return value;
    }
}
