package fixture;
public final class GetterLeaf implements GetterBase {
    public final int value;
    public GetterLeaf(int value) { this.value = value; }
    public int tag() { return value; }
}
