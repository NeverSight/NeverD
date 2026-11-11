package fixture;

public abstract class Parent<P> {
    public Parent() {}
    public abstract Base<P> getItems();
    public final Base<P> items() { return getItems(); }
}
