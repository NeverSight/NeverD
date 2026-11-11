package fixture;
public interface Api<T> { Carrier<T, ? extends Api<T>> result(); }
