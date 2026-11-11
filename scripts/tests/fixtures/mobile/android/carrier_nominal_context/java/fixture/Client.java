package fixture;
import java.io.File;
import java.util.Collection;
public class Client {
    public static <T> File call(Collection<T> values,File file,String text) throws Failure {return Provider.select(values,file,text);}
    public static <T> File full(Collection<T> values,File file,String text,boolean flag) throws Failure {return Provider.full(values,file,text,flag);}
}
