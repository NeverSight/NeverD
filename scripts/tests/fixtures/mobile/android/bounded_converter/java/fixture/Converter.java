package fixture;
public interface Converter<T,V extends Bound> { Callback<T,V> get(); }
