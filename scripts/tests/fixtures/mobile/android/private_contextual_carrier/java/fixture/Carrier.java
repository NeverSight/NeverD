package fixture;
public class Carrier<E> {
    public Object value;
    public Carrier(Object value) { this.value = value; }
    public E get() { Core.events = Core.events * 5 + 4; return (E) value; }
}
