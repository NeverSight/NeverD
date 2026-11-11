package fixture;
@SuppressWarnings("unchecked")
public class Core<T> implements Cloneable {
    public int calls;
    public Object seen;
    public T payload;
    public RuntimeException failure;
    public Core<T> clone() throws CloneNotSupportedException {
        calls++;
        seen = this;
        if (failure != null) throw failure;
        return (Core<T>)super.clone();
    }
}
