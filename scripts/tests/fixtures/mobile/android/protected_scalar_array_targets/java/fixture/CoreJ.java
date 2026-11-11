package fixture;
public final class CoreJ extends Base<long[]> {
    protected int size(long[] value) {
        calls++;
        seen = value;
        marks[0] += 3;
        if (failure != null) throw failure;
        return value.length + marks[0];
    }
    public int local(long[] value) { return size(value); }
}
