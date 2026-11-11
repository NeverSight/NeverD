package fixture;
public final class Core {
    public final long first;
    public final long second;
    public Core(long a,long b) {
        Trace.escaped=this; Trace.event(1);
        first=a; Trace.event(2);
        if(Trace.mode==1) { Trace.event(5); throw Trace.failure1; }
        second=b; Trace.event(3);
        if(Trace.mode==2) { Trace.event(6); throw Trace.failure2; }
        if(Trace.mode==3) { Trace.event(7); throw Trace.failure3; }
        Trace.event(4);
    }
}
