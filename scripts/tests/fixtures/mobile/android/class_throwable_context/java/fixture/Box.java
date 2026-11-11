package fixture;
import java.util.function.Function;
public class Box<T> {
    @SuppressWarnings("unchecked")
    public <X extends Throwable> T get(Function<String,? extends X> factory,String message)throws X {
        Trace.hit(1);Trace.receiver=this;Trace.seenFactory=factory;Trace.seenMessage=message;Trace.action.run();
        if(Trace.present){Trace.hit(3);return (T)Trace.value;}
        Trace.hit(2);X failure=factory.apply(message);Trace.thrown=failure;Trace.hit(4);throw failure;
    }
    public T plain(){return get(Trace.factory,Trace.message);}
}
