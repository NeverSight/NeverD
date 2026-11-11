package fixture;
public interface Producer<T> { T read(Token token); Token meta(); }
