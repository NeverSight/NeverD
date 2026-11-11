package fixture;
public class Cell<T> {
    public T value;
    public Cell(T value) { this.value=value; }
    public T read() { Trace.reads++;Trace.hit(2);return value; }
}
