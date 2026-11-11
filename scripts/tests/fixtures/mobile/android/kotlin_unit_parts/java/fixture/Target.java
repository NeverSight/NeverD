package fixture;

public final class Target<E> {
    public E last;
    public Watcher<E> watcher;
    public Watcher<E> callback;
    public Target() {}
    public void fire(E value) { watcher.accept(value); }
}
