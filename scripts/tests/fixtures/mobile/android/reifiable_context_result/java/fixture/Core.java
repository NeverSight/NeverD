package fixture;
public final class Core {
    private Core() {}
    public static Carrier<?> call(Factory factory, String key) {
        Factory.events = Factory.events * 5 + 3;
        Carrier<?> result = factory.get(key);
        Factory.events = Factory.events * 5 + 4;
        return result;
    }
    public static void discard(Factory factory, String key) { call(factory, key); }
    public static <R extends Carrier<?>> R typed(Factory factory, String key) {
        Factory.events = Factory.events * 5 + 3;
        R result = factory.get(key);
        Factory.events = Factory.events * 5 + 4;
        return result;
    }
    public static Leaf narrowed(Factory factory, String key) {
        Carrier<?> result = call(factory, key);
        Factory.events = Factory.events * 5 + 6;
        return (Leaf) result;
    }
}
