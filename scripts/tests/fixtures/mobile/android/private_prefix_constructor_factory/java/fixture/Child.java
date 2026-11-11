package fixture;
public final class Child extends Base<Value> {
    public boolean flag;
    public Child(boolean enabled) { super(first(enabled), second()); flag = enabled; Trace.hit(6); }
    private static Value first(boolean enabled) {
        Trace.hit(1); Trace.inputFlag = enabled;
        Value value = enabled ? Trace.left : Trace.right;
        if (value != null) value.configure(Float.intBitsToFloat(Trace.requestBits));
        return value;
    }
    private static Input second() { Trace.hit(2); Trace.prepare.run(); return Trace.tail; }
}
