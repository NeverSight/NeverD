package fixture;
import java.io.File;
import java.lang.Iterable;
public class Client {
    public static <T> File call(Provider receiver,Iterable<T> values,File file,Thread.State context) throws Failure {return receiver.select(values,file,context);}
    public static <T> File full(Provider receiver,Iterable<T> values,File file,Thread.State context,boolean flag) throws Failure {return receiver.full(values,file,context,flag);}
}
