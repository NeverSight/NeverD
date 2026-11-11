package fixture;
public final class GetterDirect implements GetterApi {
    private final java.util.List<GetterLeaf> values;
    public GetterDirect(java.util.List<GetterLeaf> values) { this.values = values; }
    public java.util.List<GetterLeaf> get() { return values; }
}
