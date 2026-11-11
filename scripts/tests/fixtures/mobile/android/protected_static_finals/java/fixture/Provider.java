package fixture;
public class Provider {
    public static Object read(int index)throws NoSuchMethodException {Trace.calls++;Trace.stage=index;Trace.hit(index);Trace.action.run();if(index==Trace.failAt)throw Trace.checked;return Trace.results[index-1];}
}
