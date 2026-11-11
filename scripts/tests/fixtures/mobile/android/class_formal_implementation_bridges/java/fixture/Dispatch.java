package fixture;
public final class Dispatch {
    public static <K extends Comparable<? super K>> boolean contains(Api<K> receiver, K value) {
        K observed = receiver.observe(value);
        K lower = receiver.lower();
        return observed.compareTo(lower) >= 0;
    }
    public static <K extends Comparable<? super K>> boolean empty(Api<K> receiver) {
        return receiver.lower() == null;
    }
}
