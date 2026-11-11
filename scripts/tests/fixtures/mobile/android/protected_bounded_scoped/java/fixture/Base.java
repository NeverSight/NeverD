package fixture;
public abstract class Base<T extends Container<V>, V> {
    public int visits;
    protected abstract void insert(T box, int index, V value);
    public void dispatch(T box, int index, V value) { visits++; insert(box, index, value); }
}
