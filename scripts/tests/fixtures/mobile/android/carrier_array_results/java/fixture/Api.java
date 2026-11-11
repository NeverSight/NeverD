package fixture;
public interface Api { <T> T[] read(int begin, int end, Class<T> token); }
