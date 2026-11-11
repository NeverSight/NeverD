package fixture;
import java.util.List;
public class Base<T> {
    public final List<T> list;
    public final int result;
    public Base(List<T> list,int result) {
        this.list=list;
        this.result=result;
        Trace.calls++;
        Trace.owner=this;
        Trace.stage=1;
        Trace.action.run();
        Trace.stage=2;
    }
}
