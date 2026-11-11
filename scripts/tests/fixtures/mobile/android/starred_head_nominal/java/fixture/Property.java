package fixture;
public interface Property<T, U> { U read(T value); void write(T value, U element); }
