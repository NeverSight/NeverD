package fixture;
import java.util.Collection;
public class Client {
    public static <T extends Collection<String>> T call(Provider receiver,T values) throws Failure {return receiver.read(values);}
    public static <T extends Collection<String>> T full(Provider receiver,T values,boolean flag) throws Failure {return Provider.transfer(receiver,values,flag);}
}
