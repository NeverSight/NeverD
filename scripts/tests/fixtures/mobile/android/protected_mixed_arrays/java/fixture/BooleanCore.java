package fixture;
public final class BooleanCore extends Base<boolean[]> {
    public int calls, index;
    public Object seen;
    public RuntimeException failure;
    protected void write(Token token, boolean[] array, int index) {
        calls++; seen = array; this.index = index;
        if (failure != null) throw failure;
        token.calls++;
        array[index] = true;
    }
}
