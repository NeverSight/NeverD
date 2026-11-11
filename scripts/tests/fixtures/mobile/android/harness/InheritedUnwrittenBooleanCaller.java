import fixture.*;
public final class InheritedUnwrittenBooleanCaller {
    public static Child create(long value,int marker,boolean delegate) { return Client.create(value,marker,delegate); }
    public static boolean read(Owner owner) { return owner.zeroFlag; }
}
