package fixture;
public final class Provider {
    public static Callback replacement;
    @SuppressWarnings("unchecked")
    private static <T> Callback<T,Unit> read(Carrier<? extends Callback<? super T,Unit>> state) {
        return (Callback<T,Unit>)state.get();
    }
    @SuppressWarnings("unchecked")
    public static Callback access(Carrier state) { return read(state); }
}
