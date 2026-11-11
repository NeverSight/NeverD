package fixture;
public final class GetterFloatBox {
    public final float value;
    public static int calls;
    public static float last;
    public static RuntimeException failure;
    public GetterFloatBox(float value) { this.value = value; }
    public static GetterFloatBox box(float value) {
        calls++; last = value;
        if (failure != null) throw failure;
        return new GetterFloatBox(value);
    }
}
