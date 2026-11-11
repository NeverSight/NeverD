package fixture;
public class Provider {
    @SuppressWarnings("unchecked")
    public static <P,R> Output<P,R> select(Input<P,R> input,Mapper<RuntimeException,Exception> context){Trace.calls++;Trace.input=input;Trace.context=context;Trace.hit(1);Trace.action.run();Trace.hit(2);return (Output<P,R>)Trace.result;}
    public static <P,R> Output<P,R> reversed(Mapper<RuntimeException,Exception> context,Input<P,R> input){return select(input,context);}
}
