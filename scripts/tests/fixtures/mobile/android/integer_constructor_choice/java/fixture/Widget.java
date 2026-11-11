package fixture;
public final class Widget {
    static { Trace.hit(1); }
    public final int kind,value;
    public final int source;
    public Widget() {
        Trace.current=this;Trace.received=null;Trace.constructors++;Trace.hit(2);
        this.kind=1;Trace.hit(3);this.source=0;this.value=Trace.seed;Trace.hit(4);
    }
    public Widget(int source) {
        Trace.current=this;Trace.received=source;Trace.constructors++;Trace.hit(5);
        this.kind=2;Trace.hit(6);this.source=source;this.value=Trace.seed;Trace.hit(7);
    }
}
