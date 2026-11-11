package fixture;

public final class Token implements Bound {
    public int value;
    public Token(int value) { this.value = value; }
    @Override public int value() { return value; }
}
