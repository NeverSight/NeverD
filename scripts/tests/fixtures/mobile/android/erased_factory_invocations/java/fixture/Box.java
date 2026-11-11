package fixture;
public final class Box<T> {
    static { Trace.hit(4); }
    public final T value;
    public Box(T value) {
        Trace.boxes++; Trace.hit(5); this.value = value;
    }
}
