package fixture;
public final class Box<I> {
    public final I value;
    public Box(I value) {
        Trace.boxes++;
        Trace.hit(7);
        this.value = value;
    }
}
