package fixture;
public final class Provider {
    public static Callback replacement;
    @SuppressWarnings("unchecked")
    private static <T> Callback<T> read(Carrier<? extends Callback<T>> state) {
        return (Callback<T>)state.get();
    }
    @SuppressWarnings("unchecked")
    public static Callback access(Carrier state) { return read(state); }
}
