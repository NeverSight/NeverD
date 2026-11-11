package fixture;
public final class FixedDirect implements FixedApi {
    public FixedBox<? super FixedTag> selected;
    public int calls;
    public void put(FixedBox<? super FixedTag> box) { selected = box; calls++; }
    public FixedBox<? super FixedTag> get() { return selected; }
}
