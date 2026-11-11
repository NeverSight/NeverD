package fixture;
public class Parent extends java.util.LinkedHashMap<String,Object> implements Reader<String> {
    public Result read(String key) {
        Trace.calls++;Trace.hit(1);Trace.actualReceiver=this;Trace.key=key;
        Trace.chosen=clone();
        Trace.observed=key.length();Trace.hit(2);Trace.action.run();Trace.hit(3);
        return Trace.result;
    }
    public Parent clone() {
        Trace.seedCalls++;Trace.hit(4);Trace.seedReceiver=this;Trace.seedArgument=Trace.key;
        Trace.seedAction.run();Trace.hit(5);return Trace.selected;
    }
}
