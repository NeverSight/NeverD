package fixture;

public abstract class DeclarationOnly {
    protected DeclarationOnly() {}
    public abstract int abstractValue(int value);
    public static native long nativeValue(long value);
}
