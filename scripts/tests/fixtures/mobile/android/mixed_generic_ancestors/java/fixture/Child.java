package fixture;

public class Child<S extends Tag, U extends Tag> extends Parent<S> {
    public S value;
    public int calls;
    public int views;
    public RuntimeException failure;
    public Child(int seed) { super(seed); }
    public S echo(S value) {
        ++calls;
        if (failure != null) throw failure;
        this.value = value;
        return this.value;
    }
    public int view(int position) {
        ++views;
        return super.view(position) + 2;
    }
    public int count() { return super.count() + 1; }
}
