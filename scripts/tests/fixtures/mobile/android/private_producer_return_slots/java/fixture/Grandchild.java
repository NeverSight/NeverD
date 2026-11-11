package fixture;
public final class Grandchild<G> extends Child<G> {
    public int levels;
    public Grandchild() { super(); }
    @Override public Leaf<G> narrowItems() {
        levels++;
        return super.narrowItems();
    }
}
