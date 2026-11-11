package fixture;
public final class Carrier {
    public byte[] value;
    public int calls;
    public int[] marker;
    public RuntimeException failure;
    public Carrier(byte[] value) { this.value=value; }
    public byte[] unbox() {
        calls++; if (marker != null) marker[0]+=3;
        if (failure != null) throw failure;
        return value;
    }
}
