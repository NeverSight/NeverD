package fixture;
public interface Api { <T extends Value> T read(Object token, int index, long stamp, double fraction); }
