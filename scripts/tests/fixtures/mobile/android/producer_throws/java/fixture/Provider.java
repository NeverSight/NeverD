package fixture;
public class Provider {
    @SuppressWarnings("unchecked")
    public <T> T read(Handler<T> handler) throws Failure {Trace.calls++;Trace.receiver=this;Trace.handler=handler;Trace.hit(1);Trace.action.run();Trace.hit(2);return (T)Trace.result;}
}
