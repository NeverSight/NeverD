package fixture;
public abstract class Token<T> {
    public abstract float read(T seed);
    public abstract void write(T seed, float value);
}
