package fixture;

public final class PlainChild extends DeprecatedBase {
    public int own;

    public PlainChild(int seed) {
        super(seed);
        own = seed * 2;
    }

    public int childValue(int value) {
        return own + value * 3 - 2;
    }
}
