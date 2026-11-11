package fixture;

public final class CheckedBound {
    private CheckedBound() {}

    private static <T, R extends Bounded & Iterable<? extends T>> boolean probe(R receiver, T value) {
        return value != null && receiver.accept((Comparable) value);
    }
}
