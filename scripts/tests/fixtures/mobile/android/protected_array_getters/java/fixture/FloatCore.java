package fixture;
public final class FloatCore extends Base<float[]> {
    public int calls, length;
    public boolean fresh;
    public float[] value, last;
    public RuntimeException failure;
    protected float[] get() {
        calls++;
        if (failure != null) throw failure;
        if (fresh) value = new float[length];
        last = value;
        return value;
    }
}
