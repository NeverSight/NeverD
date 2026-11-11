package fixture;
public final class Core {
    private Core() {}
    public static Outer.Item call(Outer.Provider provider, int index) {
        Outer.events = Outer.events * 5 + 3;
        Outer.Item result = provider.select(index);
        Outer.events = Outer.events * 5 + 4;
        return result;
    }
    public static void discard(Outer.Provider provider, int index) { call(provider, index); }
    public static <R> Outer.Item<R> typed(Outer.Provider provider, int index) {
        Outer.events = Outer.events * 5 + 3;
        Outer.Item<R> result = provider.select(index);
        Outer.events = Outer.events * 5 + 4;
        return result;
    }
}
