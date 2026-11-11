package fixture;

public class Variant<L> implements Producer<L> {
    public Cell<L, L> cell;
    public Cell<L, L>[] cells;
    public Variant(Cell<L, L> cell, Cell<L, L>[] cells) {
        this.cell = cell;
        this.cells = cells;
    }
    public Cell<L, L> upper() { return cell; }
    public Cell<L, L> lower() { return cell; }
    public Cell<L, L>[] array() { return cells; }
    public L identity(L value) { return value; }
}
