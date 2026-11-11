package fixture;
@SuppressWarnings("rawtypes")
public final class Core implements Api {
    public Object invoke(Object first, Object second, Object third) {
        invoke((Void) first, (Context) second, ((Number) third).intValue());
        return kotlin.Unit.INSTANCE;
    }
    public final void invoke(Void first, Context second, int third) {
        Trace.calls++; Trace.hit(1);
        Trace.receiver = this; Trace.first = first; Trace.second = second; Trace.mask = third;
        Trace.hit(2);
    }
}
