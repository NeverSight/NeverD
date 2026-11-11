package fixture;
public class Parent extends Base<String,Object> implements Reader<String> {
    public Result read(String key) { return extra(key); }
    public Result extra(String key) {
        Trace.calls++;Trace.hit(1);Trace.actualReceiver=this;Trace.key=key;
        Trace.observed=key.length();Trace.hit(2);Trace.action.run();Trace.hit(3);
        return Trace.result;
    }
}
