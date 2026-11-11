package fixture;
public class Core extends Parent {
    public final long after;
    public Core(long input) { this(input, Core.code, Core.wide, Core.token); }
    public Core(long input, int code, long wide, Object token) {
        super(input, code, wide, token);
        Trace.hit(8); this.after=wide; Trace.hit(9);
    }
    public Core() {
        super(Core.wide, Core.code, Core.wide, Core.token);
        Trace.hit(8); this.after=wideSnapshot; Trace.hit(9);
    }
}
