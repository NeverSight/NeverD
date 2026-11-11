package fixture;
public class Provider extends Base<String,Object> implements Tagged<String> {
    public <T> Provider accept(T value) {
        Trace.calls++;Trace.actualReceiver=this;Trace.captured=value;Trace.hit(2);
        Trace.action.run();Trace.hit(3);return Trace.result;
    }
}
