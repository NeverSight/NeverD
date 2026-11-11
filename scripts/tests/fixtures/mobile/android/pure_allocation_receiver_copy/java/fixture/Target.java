package fixture;
public final class Target {
    public final long bits;
    public final boolean enabled,active;
    public final int tag;
    public Target(long bits,boolean enabled,int tag,boolean active) {
        Trace.created=this;Trace.count++;Trace.hit(1);
        this.bits=bits;Trace.hit(2);
        this.enabled=enabled;Trace.hit(3);
        this.tag=tag;Trace.hit(4);
        this.active=active;Trace.hit(5);
    }
}
