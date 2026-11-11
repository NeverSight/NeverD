package fixture;
public abstract class Root<R> {
    public Root() {}
    public abstract Base<R> create(int lower, int upper);
}
