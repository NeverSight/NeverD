package fixture;
public class Parent extends Base<String,Object> implements Reader<String> {
    public Result read(String key) { return null; }
    @SuppressWarnings("unchecked")
    public <U extends Enum<U>> U extra(Class<U> token,String key) {
        Trace.calls++;Trace.hit(1);Trace.actualReceiver=this;Trace.key=key;Trace.token=token;
        Trace.observed=key.length();Trace.hit(2);Trace.action.run();Trace.hit(3);
        return (U)Trace.result;
    }
}
