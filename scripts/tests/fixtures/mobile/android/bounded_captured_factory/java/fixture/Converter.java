package fixture;
public interface Converter<T,V extends Vector> {
    T value();
    V convert(T value);
}
