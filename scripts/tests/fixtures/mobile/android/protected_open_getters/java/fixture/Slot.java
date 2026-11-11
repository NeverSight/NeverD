package fixture;
public final class Slot extends Broad<Core<?>> {
    public static int created;
    public int value;
    public Slot(int value) { created++; this.value = value; }
}
