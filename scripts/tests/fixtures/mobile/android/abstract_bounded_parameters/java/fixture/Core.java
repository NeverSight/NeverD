package fixture;
public class Core<E> extends Middle<E, Box<E>> {
    public int calls;
    public String marker;
    public int index;
    public Box<E> seen;
    public long wide;
    public boolean enabled;
    public E value;
    public RuntimeException failure;
    public void read(String marker, int index, Box<E> box, long wide, boolean enabled) {
        calls++; this.marker = marker; this.index = index; seen = box; this.wide = wide; this.enabled = enabled;
        if (failure != null) throw failure;
        box.value = value; box.index = index; box.wide = wide; box.enabled = enabled;
    }
    public void typed(String marker, int index, Box<E> box, long wide, boolean enabled) { read(marker, index, box, wide, enabled); }
}
