package fixture;
public class Provider extends Middle {
    @Override public <T> Provider accept(T value,boolean left,boolean right) {
        Trace.leafCalls++;Trace.hit(1);
        Provider result=(Provider)super.accept(value,left,right);
        Trace.leafAfter++;Trace.hit(6);return result;
    }
}
