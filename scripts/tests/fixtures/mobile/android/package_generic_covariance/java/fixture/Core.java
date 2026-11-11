package fixture;
public class Core extends Parent<String> {
    public Leaf<? extends Tag> selected;
    @Override public Leaf<? extends Tag> copy() {
        calls += 2;
        super.copy();
        return selected;
    }
}
