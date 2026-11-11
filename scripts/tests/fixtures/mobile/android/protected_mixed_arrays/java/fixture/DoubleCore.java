package fixture;
public final class DoubleCore extends Base<double[]> {
    public int calls, index;
    public Object seen;
    public RuntimeException failure;
    protected void write(Token token, double[] array, int index) {
        calls++; seen = array; this.index = index;
        if (failure != null) throw failure;
        token.calls++;
        array[index] = -0.0d;
    }
}
