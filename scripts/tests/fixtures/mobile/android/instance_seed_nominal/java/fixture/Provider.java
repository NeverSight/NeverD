package fixture;
public class Provider extends Middle {
    public <T> Provider accept(T value,boolean left,boolean right) {
        Trace.calls++;Trace.actualReceiver=this;Trace.captured=value;Trace.left=left;Trace.right=right;Trace.hit(2);
        Trace.action.run();Trace.hit(3);return Trace.result;
    }
}
