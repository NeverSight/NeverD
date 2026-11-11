package fixture;
public final class Core {
    public final Object first, choice, last;
    public final long stamp;
    public Core(Object seed, Object alternate) {
        Trace.partial=this;
        Trace.step(1);
        first=seed;
        Trace.step(2);
        if (!Trace.accept(seed)) throw Trace.problem(4);
        if (alternate==null) {
            Trace.step(5);
            if (Trace.inlineAbort) {
                Trace.step(6);
                throw Trace.problem(7);
            }
            choice=Trace.choose(seed);
        } else {
            if (Trace.rejectAlternate) throw Trace.problem(9);
            choice=alternate;
        }
        Trace.step(10);
        last=Trace.finish(seed);
        Trace.step(12);
        stamp=Trace.stamp;
        Trace.step(13);
        if (Trace.abortAfter) throw Trace.problem(14);
    }
}
