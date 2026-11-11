package fixture;
public class Observer {
    public static void observe(int phase) {
        Trace.observations++;
        Trace.hit(phase + 4);
        boolean valid;
        if (phase == 0) {
            valid = !Owner.aZ && Owner.aB == 0 && Owner.aC == 0 && Owner.aS == 0 && Owner.aI == 0 && Owner.aJ == 0
                && Float.floatToRawIntBits(Owner.aF) == 0 && Double.doubleToRawLongBits(Owner.aD) == 0
                && Owner.aRef == null && Owner.aArray == null;
            Trace.first = true;
        } else {
            valid = Owner.aZ == Trace.z && Owner.aB == Trace.b && Owner.aC == Trace.c && Owner.aS == Trace.s
                && Owner.aI == Trace.i && Owner.aJ == Trace.j
                && Float.floatToRawIntBits(Owner.aF) == Float.floatToRawIntBits(Trace.f)
                && Double.doubleToRawLongBits(Owner.aD) == Double.doubleToRawLongBits(Trace.d)
                && Owner.aRef == Trace.seed && Owner.aArray == Trace.array;
            Trace.second = true;
        }
        if (!valid || Owner.zzzAnchor != 7) Trace.bad++;
    }
}
