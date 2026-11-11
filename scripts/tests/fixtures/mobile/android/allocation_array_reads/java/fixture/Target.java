package fixture;
public class Target {
    public final Object reference;
    public final double wide;
    public final float narrow;
    public final long number;
    public final int integer;
    public final boolean flag;
    public final byte small;
    public final char character;
    public final short medium;

    public final int after;
    static {
        Trace.hit(9);
        Provider p = Trace.provider;
        if (p.reference != null) p.reference[0] = Trace.reference; p.wide[0] = Trace.wide; p.narrow[0] = Trace.narrow;
        p.number[0] = Trace.number; p.integer[0] = Trace.integer; p.flag[0] = Trace.flag;
        p.small[0] = Trace.small; p.character[0] = Trace.character; p.medium[0] = Trace.medium;
    }
    public Target(Object reference, double wide, float narrow, long number, int integer, boolean flag, byte small, char character, short medium, int after) {
        this.reference = reference;
        this.wide = wide;
        this.narrow = narrow;
        this.number = number;
        this.integer = integer;
        this.flag = flag;
        this.small = small;
        this.character = character;
        this.medium = medium;

        this.after = after;
        Trace.published = this; Trace.hit(8);
    }
}
