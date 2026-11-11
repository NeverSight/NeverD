import fixture.*;
public final class UnwrittenInstanceBooleanCaller {
    public static Owner create(long value,int count) { return Client.create(value,count); }
    public static boolean read(Owner owner) { return owner.zeroFlag; }
}
