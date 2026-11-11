package fixture;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class RawOwner implements RawApi {
    public RawBox<RawTag> accepted;
    public int accepts;
    public RawBox get(RawTag input) { return RawFactory.make(input); }
    public void accept(RawBox<RawTag> box) { accepted = box; accepts++; }
}
