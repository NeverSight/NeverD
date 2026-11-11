package fixture;
public class Parent<T extends Number> {
    public T first, second;
    public final long tag;
    public Parent(T first, T second, long tag) {
        Trace.hit(3);
        this.first = first;
        this.second = second;
        this.tag = tag;
        Trace.partial = this;
        Trace.action.run();
        Trace.hit(5);
    }
}
