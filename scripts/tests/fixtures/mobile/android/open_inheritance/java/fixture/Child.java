package fixture;

public class Child<I, O> extends Base<I> {
    public Cell<I, O> cell;
    public I last;
    public Option option;
    public int calls;
    public RuntimeException failure;
    public Child(Cell<I, O> cell) {
        this.cell = cell;
    }
    public void apply(I value, Option option) {
        ++calls;
        if (failure != null) throw failure;
        last = value;
        this.option = option;
        cell.first = value;
    }
    public Cell<I, O> result() {
        return cell;
    }
}
