package fixture;
public final class Caller {
    public static <T extends Comparable<? super T>, U extends Comparable<? super U>>
            boolean call(Core<T,U> receiver, T value) { return receiver.contains(value); }
}
