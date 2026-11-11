package fixture;
public final class ByteCore extends Base<byte[]> {
    public int calls, length;
    public boolean fresh;
    public byte[] value, last;
    public RuntimeException failure;
    protected byte[] get() {
        calls++;
        if (failure != null) throw failure;
        if (fresh) value = new byte[length];
        last = value;
        return value;
    }
}
