package fixture;
public final class Result<T,V extends Bound> {
    public final T first,second,third;
    public final Link<T,V> converter;
    public Result(T first,T second,T third,Link<T,V> converter) {
        Trace.partial=this;Trace.hit(3);
        this.first=first;Trace.hit(4);
        this.second=second;this.third=third;this.converter=converter;Trace.hit(5);
    }
    public T first() { return first; }
    public T second() { return second; }
    public T third() { return third; }
}
