package fixture;
public interface Producer<T> { T get(int flags, Object input, long bits); }
