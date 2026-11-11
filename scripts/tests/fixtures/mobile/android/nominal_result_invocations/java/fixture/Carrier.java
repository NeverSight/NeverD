package fixture;
@SuppressWarnings("unchecked")
public class Carrier<E> {
    public Object stored;
    public int reads;
    public E get() { reads++; return (E)stored; }
}
