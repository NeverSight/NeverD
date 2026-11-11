package fixture;
public final class Core {
    private Core() {}
    public static Outcome raw(Callback callback) { return Provider.resolve(callback); }
    public static void ignore(Callback callback) { Provider.resolve(callback); }
    public static <E> Outcome relay(Callback<E,?> callback) { return Provider.resolve(callback); }
    public static String name(Callback callback) { return Provider.resolve(callback).getImplMethodName(); }
}
