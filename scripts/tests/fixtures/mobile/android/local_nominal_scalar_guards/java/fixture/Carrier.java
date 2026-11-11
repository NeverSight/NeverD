package fixture;
public class Carrier {
    public byte byteValue;
    public long longValue;
    public int calls;
    public int[] marker;
    public RuntimeException failure;
    public Carrier(byte byteValue, long longValue) { this.byteValue = byteValue; this.longValue = longValue; }
    private void touch() {
        ++calls;
        if (marker != null) marker[0] += 3;
        if (failure != null) throw failure;
    }
    public byte readByte() { touch(); return byteValue; }
    public long readLong() { touch(); return longValue; }
}
