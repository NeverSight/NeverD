package fixture;
import java.io.File;
import java.lang.Iterable;
public class Provider {
    public <T> File select(Iterable<T> values,File file,Thread.State context) throws Failure {return full(values,file,context,false);}
    public <T> File full(Iterable<T> values,File file,Thread.State context,boolean flag) throws Failure {Trace.calls++;Trace.receiver=this;Trace.values=values;Trace.file=file;Trace.context=context;Trace.flag=flag;Trace.hit(1);Trace.action.run();Trace.hit(2);return Trace.result;}
}
