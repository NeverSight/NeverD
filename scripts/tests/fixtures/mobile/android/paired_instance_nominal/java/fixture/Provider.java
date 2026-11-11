package fixture;
import java.util.function.BiFunction;
public class Provider {
    public Provider() {}
    @SuppressWarnings("unchecked")
    public <A,B> Object bind(Class<A> first,Class<B> second,BiFunction<? super A,? super B,Boolean> callback) {
        Trace.hit(1,this,first,second,callback,null);
        Trace.accepted=callback.apply((A)Trace.firstValue,(B)Trace.secondValue);
        Object result=Trace.returned;
        Trace.hit(2,this,first,second,callback,result);
        return result;
    }
    @SuppressWarnings("unchecked")
    public <A,B> void consume(Class<A> first,Class<B> second,BiFunction<? super A,? super B,Boolean> callback) {
        Trace.hit(1,this,first,second,callback,null);
        Trace.accepted=callback.apply((A)Trace.firstValue,(B)Trace.secondValue);
        Object observed=Trace.returned;
        Trace.hit(2,this,first,second,callback,observed);
    }
    @SuppressWarnings("unchecked")
    public <A,B> long measure(Class<A> first,Class<B> second,BiFunction<? super A,? super B,Boolean> callback) {
        Trace.hit(1,this,first,second,callback,null);
        Trace.accepted=callback.apply((A)Trace.firstValue,(B)Trace.secondValue);
        long result=Trace.number;
        Trace.hit(2,this,first,second,callback,Trace.returned);
        return result;
    }
}
