import fixture.*;
public class AnonymousMemberAllocationCaller {
    public static GenericBase<Owner, Float> property() { return Owner.PROPERTY; }
    public static Float get(GenericBase<Owner, Float> property, Owner owner) { return property.get(owner); }
    public static void set(GenericBase<Owner, Float> property, Owner owner, Float value) { property.set(owner, value); }
}
