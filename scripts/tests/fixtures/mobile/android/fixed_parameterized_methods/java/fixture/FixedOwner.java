package fixture;
public final class FixedOwner implements FixedChild {
    public FixedBox<? super FixedTag> selected;
    public int calls;
    public void put(FixedBox<? super FixedTag> box) { selected = box; calls++; }
    public FixedBox<? super FixedTag> get() { return selected; }
    public <R> R echo(R input) { return input; }
}
