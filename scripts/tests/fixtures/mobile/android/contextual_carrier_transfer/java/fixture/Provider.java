package fixture;
public class Provider {
    static { Trace.hit(1); Trace.initialize.run(); Trace.hit(2); }
    @SuppressWarnings("unchecked")
    public static <E,T extends Carrier<E>> T transfer(Context context,T seed,Class<E> token) throws java.io.IOException {
        Trace.hit(3);
        Trace.context=context; Trace.seed=seed; Trace.token=token;
        context.action.run();
        Trace.hit(4);
        if(context.checked!=null) throw context.checked;
        if(Trace.useAlternate) { Trace.hit(5); return (T) context.alternate; }
        Trace.hit(6); return seed;
    }
}
