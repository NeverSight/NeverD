package fixture;
public final class IntegerCore extends Base<int[]> {
    public int calls, index;
    public Object seen;
    public RuntimeException failure;
    protected void write(Token token, int[] array, int index) {
        calls++; seen = array; this.index = index;
        if (failure != null) throw failure;
        token.calls++;
        array[index] = 0x12345678;
    }
}
