package fixture;
public class Middle extends Closed {
    @Override public <U> Middle accept(U value,boolean left,boolean right) {
        Trace.middleCalls++;Trace.hit(2);
        Middle result=(Middle)super.accept(value,left,right);
        Trace.middleAfter++;Trace.hit(5);return result;
    }
}
