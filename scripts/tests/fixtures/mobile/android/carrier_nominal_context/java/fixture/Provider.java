package fixture;
import java.io.File;
import java.util.Collection;
public class Provider {
    public static <T> File select(Collection<T> values,File file,String text) throws Failure {return full(values,file,text,false);}
    public static <T> File full(Collection<T> values,File file,String text,boolean flag) throws Failure {Trace.calls++;Trace.values=values;Trace.file=file;Trace.text=text;Trace.flag=flag;Trace.hit(1);Trace.action.run();Trace.hit(2);return Trace.result;}
}
