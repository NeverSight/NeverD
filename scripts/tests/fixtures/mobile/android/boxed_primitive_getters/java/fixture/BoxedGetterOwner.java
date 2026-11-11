package fixture;
public final class BoxedGetterOwner implements BoxedGetterApi {
    private final float narrow;
    private final double wide;
    public BoxedGetterOwner(float narrow, double wide) { this.narrow = narrow; this.wide = wide; }
    public float readFloat() { return narrow; }
    public double readDouble() { return wide; }
    public Object getFloat() { return GetterFloatBox.box(readFloat()); }
    public Object getDouble() { return GetterDoubleBox.box(readDouble()); }
    public <R, S> R echo(R input, S tag) { return input; }
}
