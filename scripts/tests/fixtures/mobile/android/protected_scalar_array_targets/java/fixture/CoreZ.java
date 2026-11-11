package fixture;
public final class CoreZ extends Base<boolean[]> {
    protected int size(boolean[] value) {
        calls++;
        seen = value;
        marks[0] += 3;
        if (failure != null) throw failure;
        return value.length + marks[0];
    }
    public int local(boolean[] value) { return size(value); }
}
