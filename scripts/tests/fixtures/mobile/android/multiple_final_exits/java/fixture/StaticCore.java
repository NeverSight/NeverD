package fixture;
public final class StaticCore {
    public static final long first;
    public static final long second;
    static {
        Trace.event(1);
        first=Trace.a; Trace.event(2);
        if(Trace.mode==1) { Trace.event(5); throw Trace.failure1; }
        second=Trace.b; Trace.event(3);
        if(Trace.mode==2) { Trace.event(6); throw Trace.failure2; }
        if(Trace.mode==3) { Trace.event(7); throw Trace.failure3; }
        Trace.event(4);
    }
}
