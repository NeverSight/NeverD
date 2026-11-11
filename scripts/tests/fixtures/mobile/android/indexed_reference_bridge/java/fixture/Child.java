package fixture;
public class Child<C> extends Counter<C> implements CountView<C> {
    public int overrides;
    public Child() {}
    @Override public C removeAt(int index) {
        overrides++;
        return super.removeAt(index);
    }
}
