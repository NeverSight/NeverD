package fixture;
public final class Core implements Api {
    public Box<?> marker;
    @Override public Object run(Box input, Context context) {
        return Token.VALUE;
    }
}
