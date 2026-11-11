package fixture;
public final class Provider {
    @SuppressWarnings("unchecked")
    public static <T,V extends Bound> Result<T,V> create(Carrier<T> input,Link<T,V> converter,T first,T second,T third,int kind) {
        Trace.hit(1);Trace.input=input;Trace.converter=converter;
        Trace.extracted=input.value();Trace.hit(2);
        if(kind==1)return (Result<T,V>)Trace.other;
        if(kind==2)return null;
        Result<T,V> result=new Result<T,V>(first,second,third,converter);
        Trace.created=result;Trace.hit(6);return result;
    }
}
