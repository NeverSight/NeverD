package fixture;
public class Closed extends Base<String,Object> implements Tagged<String> {
    public <E> Closed accept(E value,boolean left,boolean right) {
        Trace.calls++;Trace.actualReceiver=this;Trace.captured=value;Trace.left=left;Trace.right=right;Trace.hit(3);
        Trace.action.run();Trace.hit(4);return Trace.result;
    }
}
