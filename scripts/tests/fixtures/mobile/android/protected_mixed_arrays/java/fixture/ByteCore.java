package fixture;
public final class ByteCore extends Base<byte[]> {
    public int calls, index;
    public Object seen;
    public RuntimeException failure;
    protected void write(Token token, byte[] array, int index) {
        calls++; seen = array; this.index = index;
        if (failure != null) throw failure;
        token.calls++;
        array[index] = (byte)-3;
    }
}
