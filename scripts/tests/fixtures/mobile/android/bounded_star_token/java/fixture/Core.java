package fixture;
public final class Core {
    private Core() {}
    public static Carrier<?> call(Factory factory, Class token) {
        Factory.events = Factory.events * 5 + 3;
        Carrier<?> result = factory.get(token);
        Factory.events = Factory.events * 5 + 4;
        return result;
    }
    public static void discard(Factory factory, Class token) { call(factory, token); }
    public static <R extends Carrier<?>> R typed(Factory factory, Class<R> token) {
        Factory.events = Factory.events * 5 + 3;
        R result = factory.get(token);
        Factory.events = Factory.events * 5 + 4;
        return result;
    }
    public static Leaf narrowed(Factory factory, Class token) {
        Carrier<?> result = call(factory, token);
        Factory.events = Factory.events * 5 + 6;
        return (Leaf) result;
    }
}
