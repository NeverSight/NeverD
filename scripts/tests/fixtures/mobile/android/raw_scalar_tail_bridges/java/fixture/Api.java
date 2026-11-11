package fixture;
public interface Api<T> { Object emit(T value, Sink<? super Token> sink); }
