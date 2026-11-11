package fixture;
public abstract class Base<T> {
    public Base(String name) {}
    public abstract float get(T value);
    public abstract void put(T value,float number);
}
