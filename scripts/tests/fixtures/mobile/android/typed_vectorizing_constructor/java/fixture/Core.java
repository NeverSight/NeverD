package fixture;
public final class Core<T,V extends Bound> implements View<T,V> {
    public final Vector<V> vector;
    public final Link<T,V> converter;
    public final T first;
    public final V velocity;
    public Core(Vector<V> vector,Link<T,V> converter,T first,V velocity) {
        Trace.partial=this;Trace.hit(2);
        this.vector=vector;Trace.hit(3);
        this.converter=converter;this.first=first;Trace.hit(4);
        this.velocity=velocity;Trace.hit(5);
    }
    public Core(Spec<T> spec,Link<T,V> converter,T first,V velocity) {
        this(spec.vectorize(converter),converter,first,velocity);
    }
    public T first() { return first; }
    public V velocity() { return velocity; }
}
