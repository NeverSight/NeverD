import fixture.*;
public final class CapturedAllocationCaller {
    public static Owner.Child make(Owner owner, Object tag) { return owner.make(tag); }
    public static Owner.Child conditional(Owner owner, Input input, Object tag) { return owner.conditional(input, tag); }
}
