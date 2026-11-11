package fixture;
public class Core {
    public static Target create(Provider p, int index, Object input) { return new Target(p.reference[index], p.wide[index], p.narrow[index], p.number[index], p.integer[index], p.flag[index], p.small[index], p.character[index], p.medium[index], p.after()); }
    public static Target guarded(Provider p, int index, Object input) {
        try { return new Target(p.reference[index], p.wide[index], p.narrow[index], p.number[index], p.integer[index], p.flag[index], p.small[index], p.character[index], p.medium[index], p.after()); } catch (Throwable failure) { Trace.last = failure; return null; }
    }
    public static Target locked(Provider p, int index, Object input) {
        synchronized (Trace.lock) { return new Target(p.reference[index], p.wide[index], p.narrow[index], p.number[index], p.integer[index], p.flag[index], p.small[index], p.character[index], p.medium[index], p.after()); }
    }
}
