package fixture;

public class Target {
    public final long number;
    public final double wide;
    public final float narrow;
    public final byte small;
    public final short medium;
    public final char character;
    public final boolean flag;

    static {
        Trace.hit(9);
        Trace.provider.number = Trace.number;
        Trace.provider.wide = Trace.wide;
        Trace.provider.narrow = Trace.narrow;
        Trace.provider.integer = Trace.integer;
        Trace.provider.flag = Trace.flag;
    }

    public Target(long number, double wide, float narrow, byte small, short medium, char character, boolean flag) {
        this.number = number;
        this.wide = wide;
        this.narrow = narrow;
        this.small = small;
        this.medium = medium;
        this.character = character;
        this.flag = flag;
        Trace.published = this;
        Trace.hit(8);
    }
}
