package fixture;

public class Provider {
    public Value read(Value values) throws Failure {return transfer(this,values,17L,false);}
    @SuppressWarnings("unchecked")
    public static <T extends Value> T transfer(Provider receiver,T values,long count,boolean flag) throws Failure {Trace.calls++;Trace.receiver=receiver;Trace.values=values;Trace.flag=flag;Trace.count=count;Trace.hit(1);Trace.action.run();Trace.hit(2);return (T)Trace.result;}
}
