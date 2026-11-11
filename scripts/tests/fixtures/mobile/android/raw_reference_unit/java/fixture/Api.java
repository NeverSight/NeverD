package fixture;
public interface Api<A,B,C,R> extends Marker<R> { R invoke(A first, B second, C third); }
