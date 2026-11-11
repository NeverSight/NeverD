package fixture;
public final class CoreF extends Base<float[]> {
    protected int size(float[] value) {
        calls++;
        seen = value;
        marks[0] += 3;
        if (failure != null) throw failure;
        return value.length + marks[0];
    }
    public int local(float[] value) { return size(value); }
}
