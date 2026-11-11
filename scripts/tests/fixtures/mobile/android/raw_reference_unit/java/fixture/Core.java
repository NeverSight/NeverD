package fixture;
@SuppressWarnings("rawtypes")
public final class Core implements Api {
    public Object invoke(Object first, Object second, Object third) {
        invoke((Throwable) first, (Void) second, (Context) third);
        return kotlin.Unit.INSTANCE;
    }
    public final void invoke(Throwable first, Void second, Context third) {
        Trace.calls++; Trace.hit(1);
        Trace.receiver = this; Trace.first = first; Trace.second = second; Trace.third = third;
        Trace.hit(2);
    }
}
