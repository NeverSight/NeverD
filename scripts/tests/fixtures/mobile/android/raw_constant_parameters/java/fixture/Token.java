package fixture;
public final class Token {
    public static final Token VALUE = new Token();
    public int value;
    private Token() {
        State.initializations++;
        if (State.fail) throw State.failure;
        value = 17;
    }
}
