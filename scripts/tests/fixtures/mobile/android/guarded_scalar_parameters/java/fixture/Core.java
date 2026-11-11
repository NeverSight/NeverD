package fixture;
public final class Core implements Api<String>, Marker {
    public long expectedLong; public double expectedDouble;
    public long lastLong; public double lastDouble;
    public int calls; public int observed; public int[] marker; public RuntimeException failure;
    public Core(long expectedLong, double expectedDouble) { this.expectedLong = expectedLong; this.expectedDouble = expectedDouble; }
    private void touch() {
        calls++; observed = marker == null ? -5 : marker[0];
        if (marker != null) marker[0] += 7;
        if (failure != null) throw failure;
    }
    public boolean checkLong(long input) { lastLong = input; touch(); return input == expectedLong; }
    public boolean checkDouble(double input) { lastDouble = input; touch(); return input == expectedDouble; }
    public final boolean contains(Object input) {
        if (!(input instanceof LongBox)) return false;
        return checkLong(((LongBox) input).unbox());
    }
    public final boolean matches(Object input) {
        if (!(input instanceof DoubleBox)) return false;
        return checkDouble(((DoubleBox) input).unbox());
    }
    public <R, S> R echo(R input, S tag) { return input; }
}
