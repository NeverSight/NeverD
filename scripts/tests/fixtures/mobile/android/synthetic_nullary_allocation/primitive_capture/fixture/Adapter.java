package fixture;
import java.io.Serializable;
public final class Adapter implements Maker,Serializable {
    public final boolean flag;
    public Adapter(boolean flag){this.flag=flag;Trace.adapters++;Trace.adapter=this;Trace.hit(1);Trace.allocateAction.run();}
    public Object make(){Trace.calls++;Trace.hit(6);Object result=Trace.result;Trace.makeAction.run();return result;}
}
