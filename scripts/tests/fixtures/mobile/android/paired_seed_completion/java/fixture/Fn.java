package fixture;
public interface Fn<A, B, C, R> extends Marker<R> { R apply(A left, B right, C completion); }
