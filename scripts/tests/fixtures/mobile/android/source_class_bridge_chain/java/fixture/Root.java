package fixture;

public abstract class Root<T> {
    public Root() {}
    public abstract T parse(String text);
}
