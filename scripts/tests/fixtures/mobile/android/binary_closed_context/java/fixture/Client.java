package fixture;
public class Client {
    public static <P,R> Output<P,R> call(Input<P,R> input,Mapper<RuntimeException,Exception> context){return Provider.select(input,context);}
    public static <P,R> Output<P,R> reversed(Mapper<RuntimeException,Exception> context,Input<P,R> input){return Provider.reversed(context,input);}
}
