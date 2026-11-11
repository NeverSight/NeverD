package fixture;
public final class Core implements Api<String>, Marker {
    public int integer; public long wide; public float narrow; public double decimal;
    public int calls; public int observed; public int[] marker;
    public RuntimeException failure;
    public Core(int integer, long wide, float narrow, double decimal) {
        this.integer = integer; this.wide = wide; this.narrow = narrow; this.decimal = decimal;
    }
    private void touch() {
        calls++; observed = marker == null ? -7 : marker[0];
        if (marker != null) marker[0] += 3;
        if (failure != null) throw failure;
    }
    public int getSize() { touch(); return integer; }
    public long getSpan() { touch(); return wide; }
    public float getRatio() { touch(); return narrow; }
    public double getWeight() { touch(); return decimal; }
    public int size() { return getSize(); }
    public long span() { return getSpan(); }
    public float ratio() { return getRatio(); }
    public double weight() { return getWeight(); }
    public <R, S> R echo(R input, S tag) { return input; }
}
