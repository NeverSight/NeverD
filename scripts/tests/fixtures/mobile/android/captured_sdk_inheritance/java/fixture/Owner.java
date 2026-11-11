package fixture;

public final class Owner<T> {
    public T value;
    public int reads;
    public java.util.Iterator<T> current;
    public Owner(T value) { this.value = value; }
    public View create() { return new View(); }
    public final class View extends java.util.AbstractCollection<T> {
        public View() { }
        public int size() { return 1; }
        public java.util.Iterator<T> iterator() {
            ++reads;
            return current;
        }
    }
}
