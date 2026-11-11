package fixture;
public abstract class Parent<T> {
    public Parent() {}
    public abstract Base<T> create();
}
