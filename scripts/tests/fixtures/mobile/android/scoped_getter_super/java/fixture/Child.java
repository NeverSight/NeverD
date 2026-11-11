package fixture;
public final class Child<K,V,T> extends Parent<K,V,T> {
    static { Trace.hit(9); }
    public final Builder<K,V> builder;
    public final int stamp;
    public Child(Builder<K,V> builder, Path<K,V,T>[] path) {
        super(builder.fetch(), path);
        this.builder = builder;
        this.stamp = builder.stamp();
        Trace.hit(7);
    }
}
