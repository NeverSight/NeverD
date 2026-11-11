package fixture;
public final class Observer {
    public static void observe() {
        Trace.observations++; Trace.hit(4);
        if (Owner.zeroZ || Owner.zeroB != 0 || Owner.zeroC != 0 || Owner.zeroS != 0 ||
            Owner.zeroI != 0 || Owner.zeroJ != 0L || Float.floatToRawIntBits(Owner.zeroF) != 0 ||
            Double.doubleToRawLongBits(Owner.zeroD) != 0L || Owner.zeroRef != null ||
            Owner.zeroText != null || Owner.zeroArray != null || Owner.zeroList != null)
            Trace.bad = 1;
        if (Trace.observations == 1) Trace.first = Parent.marker;
        else Trace.second = Parent.marker;
    }
}
