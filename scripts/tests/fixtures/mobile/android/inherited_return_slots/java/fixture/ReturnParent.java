package fixture;
public class ReturnParent<T> implements RootBase<T> {
    public ReturnParent() {}
    public boolean remove(Object key, Object value) { return RootBase.super.remove(key,value); }
    public T put(T value) { return RootBase.super.put(value); }
    public void clear() { RootBase.super.clear(); }
}
