package fixture;
public abstract class Parent<T> {
    abstract int scalar(Object marker, T input, int number, float single, long integer, double decimal);
    abstract float single(Object marker, T input, int number, float single, long integer, double decimal);
    abstract long wide(Object marker, T input, int number, float single, long integer, double decimal);
    abstract double decimal(Object marker, T input, int number, float single, long integer, double decimal);
}
