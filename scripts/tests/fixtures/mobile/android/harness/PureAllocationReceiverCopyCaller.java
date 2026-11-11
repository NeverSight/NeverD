import fixture.*;
public final class PureAllocationReceiverCopyCaller {
    public static Target call(long bits,boolean enabled,int tag,boolean active) { return Client.make(bits,enabled,tag,active); }
}
