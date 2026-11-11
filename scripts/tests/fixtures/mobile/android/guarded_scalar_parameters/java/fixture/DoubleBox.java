package fixture;
public final class DoubleBox {
    public double value; public int calls; public int observed; public int[] marker;
    public RuntimeException failure;
    public DoubleBox(double value) { this.value = value; }
    public double unbox() {
        calls++; observed = marker == null ? -3 : marker[0];
        if (marker != null) marker[0] += 3;
        if (failure != null) throw failure;
        return value;
    }
}
