package fixture;
public interface Consumer<T> { void write(Token token, T value); Token meta(); }
