package fixture;
public interface Api { <T extends Value> Carrier<T> read(Object token, int index, long stamp, double fraction); }
