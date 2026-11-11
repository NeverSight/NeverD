package fixture;
public abstract class Bound<A, B extends Parent<A>> {
    public Bound() {}
    public A call(B receiver) { return receiver.get(); }
}
