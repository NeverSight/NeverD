package fixture;
public final class CoreI extends Base<int[]> {
    protected int size(int[] value) {
        calls++;
        seen = value;
        marks[0] += 3;
        if (failure != null) throw failure;
        return value.length + marks[0];
    }
    public int local(int[] value) { return size(value); }
}
