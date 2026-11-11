package fixture;
import java.util.Calendar;
public class Core {
    public Appendable call(Calendar context,Appendable value){return select(context,value);}
    public StringBuilder boxed(Calendar context,StringBuilder value){return select(context,value);}
    @SuppressWarnings("unchecked")
    private <B extends Appendable> B select(Calendar context,B value){Trace.calls++;Trace.receiver=this;Trace.calendar=context;Trace.value=value;Trace.hit(1);Trace.action.run();Trace.hit(2);return (B)Trace.result;}
}
