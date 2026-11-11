import fixture.*;
public final class ClosedInterfaceSuperCaller {
    public static Result call(Host.Child receiver, Item item) {
        return receiver.choose(item);
    }
}
