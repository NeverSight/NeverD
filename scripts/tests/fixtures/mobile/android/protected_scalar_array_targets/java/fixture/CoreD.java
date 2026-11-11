package fixture;
public final class CoreD extends Base<double[]> {
    protected int size(double[] value) {
        calls++;
        seen = value;
        marks[0] += 3;
        if (failure != null) throw failure;
        return value.length + marks[0];
    }
    public int local(double[] value) { return size(value); }
}
