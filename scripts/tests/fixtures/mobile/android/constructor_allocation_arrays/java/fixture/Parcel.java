package fixture;
public final class Parcel {
    public final Object[] values;
    static { Trace.hit(1, null); }
    public Parcel(Object[] values) {
        Trace.array = values;
        Trace.hit(2, values);
        this.values = values;
        Trace.parcel = this;
    }
}
