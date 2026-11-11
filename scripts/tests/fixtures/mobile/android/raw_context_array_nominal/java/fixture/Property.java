package fixture;
public interface Property<A, B> { B read(A value); void write(A value, B element); }
