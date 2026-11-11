package fixture;
public final class CoreC extends Base<char[]> {
    protected int size(char[] value) {
        calls++;
        seen = value;
        marks[0] += 3;
        if (failure != null) throw failure;
        return value.length + marks[0];
    }
    public int local(char[] value) { return size(value); }
}
