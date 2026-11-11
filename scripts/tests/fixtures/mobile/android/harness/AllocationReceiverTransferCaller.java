import fixture.*;
public final class AllocationReceiverTransferCaller {
    public static Target call(Object first, Object second, Object third, Object fourth, boolean option, Owner owner) {
        return Core.make(first, second, third, fourth, option, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, owner);
    }
}
