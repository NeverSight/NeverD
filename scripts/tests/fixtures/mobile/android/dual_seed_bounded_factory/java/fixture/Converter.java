package fixture;
public interface Converter<T,V extends Bound> { V convert(T value); }
