package fixture;
public class Child<E> extends Counter<E> implements CountView<E> {
    public int overrides;
    public Child() {}
    @Override public int getCount() {
        overrides++;
        return super.getCount() + 10;
    }
}
