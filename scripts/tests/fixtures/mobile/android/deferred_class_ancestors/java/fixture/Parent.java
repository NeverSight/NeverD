package fixture;
public abstract class Parent<L,R> {
    public Parent() {}
    public abstract R current();
    public int current(int ignored) { return 17; }
}
