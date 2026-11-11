import fixture.*;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class MemberFormalCaller {
    public static Owner.Item raw(Owner owner, Owner.Item input) { return owner.echo(input); }
    public static MemberFormalHarness.Payload checked(Owner<MemberFormalHarness.Payload> owner, Owner.Item input) {
        return (MemberFormalHarness.Payload)((Owner)owner).echo(input);
    }
}
