package fixture;

public enum Mode {
    FIRST(3), SECOND(7);
    private final int bias;
    private static final EntryTable TABLE = EntryTable.create(values());
    private Mode(int bias) { this.bias = bias; }
    public int score(int value) { return value + bias; }
    public static EntryTable entries() { return TABLE; }
}
