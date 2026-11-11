package fixture;
public class Parent implements Reader<Value> {
    public Value read(float fraction,Value left,Value right) {
        Trace.parents++;Trace.hit(2);Trace.actualReceiver=this;Trace.fraction=fraction;Trace.left=left;Trace.right=right;
        Trace.observedLeft=left.code;Trace.hit(3);Trace.observedRight=right.code;Trace.hit(4);
        Trace.action.run();Trace.hit(5);return Trace.result;
    }
}
