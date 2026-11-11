package fixture;
public interface Spec<T> { <R extends Bound> Vector<R> vectorize(Link<T,R> converter); }
