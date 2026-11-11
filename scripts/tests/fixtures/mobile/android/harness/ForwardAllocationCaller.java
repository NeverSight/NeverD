import fixture.*;
public final class ForwardAllocationCaller {
    public static Target make(int selector) { return Client.make(selector); }
}
