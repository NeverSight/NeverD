package fixture;

public abstract class Slot<T> {
    public final int initial;
    public Slot(int initial) { this.initial = initial; }
    public abstract T read(Cell cell);
    public abstract void write(Cell cell, T value);
    public int bump(int input) { return input + 1; }
}
