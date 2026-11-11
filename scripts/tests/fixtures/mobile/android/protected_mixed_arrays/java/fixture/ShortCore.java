package fixture;
public final class ShortCore extends Base<short[]> {
    public int calls, index;
    public Object seen;
    public RuntimeException failure;
    protected void write(Token token, short[] array, int index) {
        calls++; seen = array; this.index = index;
        if (failure != null) throw failure;
        token.calls++;
        array[index] = (short)-257;
    }
}
