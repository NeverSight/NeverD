package fixture;
public final class CoreB extends Base<byte[]> {
    protected int size(byte[] value) {
        calls++;
        seen = value;
        marks[0] += 3;
        if (failure != null) throw failure;
        return value.length + marks[0];
    }
    public int local(byte[] value) { return size(value); }
}
