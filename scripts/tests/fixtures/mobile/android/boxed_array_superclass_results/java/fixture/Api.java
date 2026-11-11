package fixture;
public interface Api<T> { T get(); default T at() { return get(); } }
