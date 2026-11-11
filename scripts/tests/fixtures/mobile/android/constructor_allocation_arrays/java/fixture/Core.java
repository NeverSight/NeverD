package fixture;
public final class Core {
    public final Object seed;
    public final Parcel parcel;
    public Core(Object seed, int size) { this(seed, new Parcel(new Object[size])); }
    private Core(Object seed, Parcel parcel) {
        Trace.hit(3, parcel);
        this.seed = seed;
        this.parcel = parcel;
        Trace.core = this;
        Trace.hit(4, this);
        Trace.after = true;
    }
}
