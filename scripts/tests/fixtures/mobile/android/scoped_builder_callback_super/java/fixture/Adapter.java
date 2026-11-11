package fixture;
import java.util.function.Function;
import java.io.Serializable;
public final class Adapter implements Function,Serializable {
    public Adapter(){Trace.adapters++;Trace.adapter=this;Trace.hit(2);Trace.allocateAction.run();}
    public Object apply(Object input){Trace.calls++;Trace.callbackInput=input;Trace.hit(4);Object result=Trace.callbackResult;Trace.callbackAction.run();return result;}
}
