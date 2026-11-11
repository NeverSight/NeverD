package fixture;
public final class Core {
    public final int result;
    public final Object selected;
    public Core(int result,Object selected) {
        Trace.partial=this;Trace.step(3);
        this.result=result;this.selected=selected;Trace.step(4);
    }
    public Core(long first,long second) { this(first<second ? -1 : first==second ? 0 : 1,null); }
    public Core(float first,float second) { this(first<second ? -1 : first==second ? 0 : 1,null); }
    public Core(double first,double second) { this(first<second ? -1 : first==second ? 0 : 1,null); }
    public Core(double input) {
        this(Trace.choose(input)==2.0 ? 7 : 9,
             Trace.choose(input)==2.0 ? Trace.identity : new Value(input));
    }
    public Core(String marker,double input) { this(input); }
}
