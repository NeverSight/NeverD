package fixture;
public class Child<C> extends Parent<C> {
    public int overrides;
    public Child() { super(); }
    @Override public Leaf<C> narrowItems() {
        overrides++;
        return super.narrowItems();
    }
}
