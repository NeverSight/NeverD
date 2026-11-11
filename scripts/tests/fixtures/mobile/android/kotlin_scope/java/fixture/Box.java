package fixture;
public class Box<T> {
    public T value;
    public Watcher<T> watcher;
    public Box() {}
    public void fire() { watcher.accept(value); }
}
