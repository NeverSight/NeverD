package fixture;
public interface Api<T> extends Consumer<T>, Producer<T> { Token meta(); }
