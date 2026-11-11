package fixture;
public final class Widget {
    static { Trace.hit(1); }
    public final int kind,value;
    public final Widget source;
    public Widget() {
        Trace.current=this;Trace.received=null;Trace.constructors++;Trace.hit(2);
        this.kind=1;Trace.hit(3);this.source=null;this.value=Trace.seed;Trace.hit(4);
    }
    public Widget(Widget source) {
        Trace.current=this;Trace.received=source;Trace.constructors++;Trace.hit(5);
        this.kind=2;Trace.hit(6);this.source=source;this.value=Trace.seed;Trace.hit(7);
    }
}
