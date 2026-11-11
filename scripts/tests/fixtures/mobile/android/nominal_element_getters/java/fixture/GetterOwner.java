package fixture;
public final class GetterOwner implements GetterChild {
    private final java.util.List<GetterLeaf> values;
    public GetterOwner(java.util.List<GetterLeaf> values) { this.values = values; }
    public java.util.List<GetterLeaf> get() { return values; }
    public <R> R echo(R input) { return input; }
}
