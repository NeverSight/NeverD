package fixture;
public abstract class Root<T extends Wide> {
    public abstract void accept(T value, long stamp, Object note);
}
