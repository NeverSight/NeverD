package fixture;

public final class Owner<T> {
    public T value;
    public int calls;
    public int marker;
    public RuntimeException failure;
    public Owner(T value) { this.value = value; }
    public Base base(int tag) { return new Base(tag); }
    public Child child(int tag) { return new Child(tag); }
    public class Base implements Cursor<T> {
        public final int tag;
        public Base(int tag) { this.tag = tag; }
        private void touch() {
            ++calls;
            marker = marker * 3 + tag;
            if (failure != null) throw failure;
        }
        public boolean hasNext() { touch(); return value != null; }
        public T next() { touch(); return value; }
        public <T> T[] echo(T[] values) { touch(); return values; }
    }
    public final class Child extends Base implements Extended<T> {
        public Child(int tag) { super(tag); }
        public void accept(T input) {
            ++calls;
            marker = marker * 5 + tag;
            value = input;
            if (failure != null) throw failure;
        }
    }
}
