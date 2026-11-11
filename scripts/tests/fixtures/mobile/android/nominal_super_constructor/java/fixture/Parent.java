package fixture;
public class Parent<T> {
    public final T value;
    public final long stamp;
    public Parent(T value,long stamp) {
        Trace.parentPartial=this;
        Trace.step(1);
        this.value=value;this.stamp=stamp;
        Trace.seen=value;
        Trace.step(2);
    }
    public T getValue(){return value;}
}
