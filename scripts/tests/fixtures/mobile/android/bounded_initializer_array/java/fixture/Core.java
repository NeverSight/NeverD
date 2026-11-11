package fixture;
public final class Core {
    private Core() {}
    public static Bound call(Factory factory, Token token, Context context, Entry[] entries) {
        Factory.events = Factory.events * 5 + 3;
        Bound result = factory.create(token, context, entries);
        Factory.events = Factory.events * 5 + 4;
        return result;
    }
    public static void discard(Factory factory, Token token, Context context, Entry[] entries) {
        call(factory, token, context, entries);
    }
    public static <R extends Bound> R typed(Factory factory, Token<R> token, Context context, Entry<?>[] entries) {
        Factory.events = Factory.events * 5 + 3;
        R result = factory.create(token, context, entries);
        Factory.events = Factory.events * 5 + 4;
        return result;
    }
}
