package fixture;
public interface Callback<T,R> extends Functor<R> { R invoke(T value); }
