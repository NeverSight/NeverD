package fixture;
import java.util.Collection;
public class Provider {
    public Collection<String> read(Collection<String> values) throws Failure {return transfer(this,values,false);}
    @SuppressWarnings("unchecked")
    public static <T extends Collection<String>> T transfer(Provider receiver,T values,boolean flag) throws Failure {Trace.calls++;Trace.receiver=receiver;Trace.values=values;Trace.flag=flag;Trace.hit(1);Trace.action.run();Trace.hit(2);return (T)Trace.result;}
}
