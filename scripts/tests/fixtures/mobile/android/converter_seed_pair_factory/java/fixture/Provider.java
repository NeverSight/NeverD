package fixture;
public final class Provider {
    @SuppressWarnings("unchecked")
    public static <T,V extends Bound> Result<T,V> create(Link<T,V> converter,T first,T second,long firstTime,long lastTime,boolean running) {
        Trace.hit(1);Trace.converter=converter;Trace.extracted=first;Trace.hit(2);
        if(Trace.kind==1)return (Result<T,V>)Trace.other;
        if(Trace.kind==2)return null;
        Result<T,V> result=new Result<T,V>(first,second,firstTime,lastTime,running,converter);
        Trace.created=result;Trace.hit(6);return result;
    }
}
