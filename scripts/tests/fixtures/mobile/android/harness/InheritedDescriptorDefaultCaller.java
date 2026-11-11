import fixture.*;

public final class InheritedDescriptorDefaultCaller {
    public static Object call(Child receiver, Object value) {
        return Child.DefaultImpls.find(receiver, value);
    }
}
