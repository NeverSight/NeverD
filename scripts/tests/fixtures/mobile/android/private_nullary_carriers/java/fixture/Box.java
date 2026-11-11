package fixture;
final class Box<T> implements View<T> {
    static {Trace.hit(3);}
    public final float[] values;
    public final T value;
    public Box(float[] values,T value) {
        Trace.partial=this;Trace.hit(4);
        this.values=values;this.value=value;Trace.hit(5);
    }
    public T get() {Trace.hit(7);return value;}
}
