package fixture;
@SuppressWarnings({"rawtypes","unchecked"})
public final class Core {
    public static Reply call(Provider owner, Reply incoming, Cell cell, Callback callback, boolean flag, float scale, long bits) {
        return Provider.entry(owner,incoming,cell,callback,flag,scale,bits);
    }
}
