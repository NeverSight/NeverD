package fixture;
public abstract class Parent<T extends Base> {
    public Parent() {}
    public abstract T create();
}
