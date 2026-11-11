import fixture.*;
@SuppressWarnings({"rawtypes","unchecked"})
public final class PrivateAnchoredNominalCaller {
    public static Reply call(Provider owner, Reply incoming, Cell cell, Callback callback, boolean flag, float scale, long bits) {
        return Core.call(owner,incoming,cell,callback,flag,scale,bits);
    }
}
