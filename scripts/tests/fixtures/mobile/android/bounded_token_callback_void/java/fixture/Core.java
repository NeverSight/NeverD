package fixture;
public final class Core {
    private Core() {}
    public static void call(Factory factory, Token token, Callback callback) {
        Factory.events = Factory.events * 5 + 3;
        factory.accept(token, callback);
        Factory.events = Factory.events * 5 + 4;
    }
    public static void ignored(Factory factory, Token token, Callback callback) {
        call(factory, token, callback);
    }
    public static <R extends Bound> void typed(Factory factory, Token<R> token,
            Callback<? super Context, ? extends R> callback) {
        Factory.events = Factory.events * 5 + 3;
        factory.accept(token, callback);
        Factory.events = Factory.events * 5 + 4;
    }
}
