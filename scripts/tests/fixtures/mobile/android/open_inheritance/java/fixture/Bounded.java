package fixture;

public class Bounded<T extends Tag> extends Base<T> {
    public Cell<T, Object> cell;
    public T last;
    public Option option;
    public int calls;
    public RuntimeException failure;
    public Bounded(Cell<T, Object> cell) {
        this.cell = cell;
    }
    public void apply(T value, Option option) {
        ++calls;
        if (failure != null) throw failure;
        last = value;
        this.option = option;
        cell.first = value;
    }
    public Cell<T, Object> result() {
        return cell;
    }
}
