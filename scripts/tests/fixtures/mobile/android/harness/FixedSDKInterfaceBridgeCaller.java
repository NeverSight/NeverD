package fixture;
public final class FixedSDKInterfaceBridgeCaller {
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static int raw(Contract receiver, Object value) {
        return receiver.dispatch(value);
    }
}
