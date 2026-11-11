package fixture;
@SuppressWarnings("unchecked")
public class Broken<T> {
    public int calls;
    public Object seen;
    public T payload;
    public RuntimeException failure;
    public Broken<T> clone() throws CloneNotSupportedException {
        calls++;
        seen = this;
        if (failure != null) throw failure;
        return (Broken<T>)super.clone();
    }
}
