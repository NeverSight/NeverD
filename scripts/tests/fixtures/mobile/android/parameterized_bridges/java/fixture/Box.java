package fixture;

public class Box<T> {
    public T value;
    public int reads;
    public int writes;
    public Box(T value) { this.value = value; }
    public T get() { ++reads; return value; }
    public void set(T value) { ++writes; this.value = value; }
}
