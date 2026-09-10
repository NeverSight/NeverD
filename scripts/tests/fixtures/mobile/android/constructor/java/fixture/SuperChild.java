package fixture;

public final class SuperChild extends PlainBase {
    public static int bodyCalls;
    public int marker;

    public <T extends Object & CharSequence> SuperChild(T value, int marker) {
        super((Object) value);
        bodyCalls++;
        this.marker = marker;
    }
}
