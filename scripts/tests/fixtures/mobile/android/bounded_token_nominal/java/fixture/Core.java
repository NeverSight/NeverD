package fixture;
public final class Core {
    private Core() {}
    public static String call(Factory factory, Token token) {
        Factory.events = Factory.events * 5 + 3;
        String result = factory.key(token);
        Factory.events = Factory.events * 5 + 4;
        return result;
    }
    public static void discard(Factory factory, Token token) { call(factory, token); }
    public static <R extends Bound> String typed(Factory factory, Token<R> token) {
        Factory.events = Factory.events * 5 + 3;
        String result = factory.key(token);
        Factory.events = Factory.events * 5 + 4;
        return result;
    }
}
