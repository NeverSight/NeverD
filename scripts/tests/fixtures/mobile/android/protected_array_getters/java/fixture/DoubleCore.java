package fixture;
public final class DoubleCore extends Base<double[]> {
    public int calls, length;
    public boolean fresh;
    public double[] value, last;
    public RuntimeException failure;
    protected double[] get() {
        calls++;
        if (failure != null) throw failure;
        if (fresh) value = new double[length];
        last = value;
        return value;
    }
}
