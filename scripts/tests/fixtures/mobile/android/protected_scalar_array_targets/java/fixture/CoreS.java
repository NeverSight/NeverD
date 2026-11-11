package fixture;
public final class CoreS extends Base<short[]> {
    protected int size(short[] value) {
        calls++;
        seen = value;
        marks[0] += 3;
        if (failure != null) throw failure;
        return value.length + marks[0];
    }
    public int local(short[] value) { return size(value); }
}
