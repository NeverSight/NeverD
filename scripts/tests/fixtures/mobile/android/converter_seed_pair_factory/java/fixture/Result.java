package fixture;
public final class Result<T,V extends Bound> {
    public final T first,second;
    public final long firstTime,lastTime;
    public final boolean running;
    public final Link<T,V> converter;
    public Result(T first,T second,long firstTime,long lastTime,boolean running,Link<T,V> converter) {
        Trace.partial=this;Trace.hit(3);
        this.first=first;Trace.hit(4);
        this.second=second;this.firstTime=firstTime;this.lastTime=lastTime;
        this.running=running;this.converter=converter;Trace.hit(5);
    }
    public T first() { return first; }
    public T second() { return second; }
}
