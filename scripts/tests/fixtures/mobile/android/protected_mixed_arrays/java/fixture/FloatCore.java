package fixture;
public final class FloatCore extends Base<float[]> {
    public int calls, index;
    public Object seen;
    public RuntimeException failure;
    protected void write(Token token, float[] array, int index) {
        calls++; seen = array; this.index = index;
        if (failure != null) throw failure;
        token.calls++;
        array[index] = -0.0f;
    }
}
