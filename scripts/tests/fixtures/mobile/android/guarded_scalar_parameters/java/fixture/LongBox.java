package fixture;
public final class LongBox {
    public long value; public int calls; public int observed; public int[] marker;
    public RuntimeException failure;
    public LongBox(long value) { this.value = value; }
    public long unbox() {
        calls++; observed = marker == null ? -3 : marker[0];
        if (marker != null) marker[0] += 3;
        if (failure != null) throw failure;
        return value;
    }
}
