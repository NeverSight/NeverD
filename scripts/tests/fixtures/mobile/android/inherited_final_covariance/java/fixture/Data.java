package fixture;
public class Data<T> implements Narrow<T> {
    public int marker;
    public Object object;
    public int mark() { return marker; }
    public Object alias() { return object; }
}
