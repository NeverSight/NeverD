package fixture;
public abstract class Parent<P> implements CountView<P> {
    public Parent() {}
    public abstract int getCount();
    public final int size() { return getCount(); }
}
