package fixture;
public final class GetterDoubleBox {
    public final double value;
    public static int calls;
    public static double last;
    public static RuntimeException failure;
    public GetterDoubleBox(double value) { this.value = value; }
    public static GetterDoubleBox box(double value) {
        calls++; last = value;
        if (failure != null) throw failure;
        return new GetterDoubleBox(value);
    }
}
