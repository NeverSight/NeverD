import fixture.Function;
public final class CarrierTargetNamesCaller {
    public static Object call(Function target, Object first, Object second) {
        return target.invoke(first, second);
    }
}
