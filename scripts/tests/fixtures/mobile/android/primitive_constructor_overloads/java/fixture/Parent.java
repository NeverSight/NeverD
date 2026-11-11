package fixture;
public class Parent<K, V> {
    public final int selected;
    public final long number;
    static { Trace.seed = 100; Trace.hit(9); }
    public Parent(int value) { selected = 1; number = value; Trace.last = this; Trace.hit(1); }
    public Parent(long value) { selected = 3; number = value; Trace.last = this; Trace.hit(3); }
    public Parent(Parent<? extends K, ? extends V> ignored) { selected = 2; number = -2; Trace.hit(2); }
    public <T> Parent(T ignored) { selected = 4; number = -4; Trace.hit(4); }
    public <T> Parent(T... ignored) { selected = 7; number = -7; Trace.hit(7); }
}
