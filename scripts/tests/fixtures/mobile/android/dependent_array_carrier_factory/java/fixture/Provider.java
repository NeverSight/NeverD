package fixture;
public final class Provider {
    private Provider() {}
    @SuppressWarnings("unchecked")
    public static <T, E extends T> Carrier<E[]> make(Token<T> token, Carrier<E> input) {
        Trace.hit(1, token, input, null, null);
        Object first = token.read();
        Trace.hit(2, token, input, first, null);
        Object second = input.read();
        Trace.hit(3, token, input, second, null);
        Carrier<E[]> result = (Carrier<E[]>)Trace.output;
        Trace.hit(4, token, input, second, result);
        return result;
    }
}
