package fixture;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class Core {
    private Core() {}
    public static Factory call(Token left, Token right, Context context) {
        return Factory.make(left, right, context);
    }
    public static Factory<Base, Base> typed(Token<Base> left, Token<Base> right, Context context) {
        return Factory.make(left, right, context);
    }
}
