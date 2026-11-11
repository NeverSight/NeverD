package fixture;

public class Client {
    public static <T extends Value> T call(Provider receiver,T values) throws Failure {return (T)receiver.read(values);}
    public static <T extends Value> T full(Provider receiver,T values,long count,boolean flag) throws Failure {return Provider.transfer(receiver,values,count,flag);}
}
