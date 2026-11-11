package fixture;
public class Base<T> {
    public int visits;
    protected void accept(T value) {}
    public void dispatch(T value) { visits++; accept(value); }
}
