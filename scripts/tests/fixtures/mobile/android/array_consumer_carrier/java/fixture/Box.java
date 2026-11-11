package fixture;
public final class Box<T> {
    public final T first, second;
    public final int decision;
    public Box(T first, T second, int decision) {
        this.first = first; this.second = second; this.decision = decision;
    }
}
