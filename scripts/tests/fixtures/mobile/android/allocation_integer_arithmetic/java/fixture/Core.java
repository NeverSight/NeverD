package fixture;
public final class Core {
    public static Target make(int x, int y, int delta) {
        return new Target(x - delta, y - delta, x + delta, y + delta);
    }
    public static Target bias(int x, int y) {
        return new Target(x - 13, y - 13, x + 13, y + 13);
    }
}
