import fixture.*;
public final class CapturedClosedDelegationCaller {
    public static Object call(Owner owner,int index,long bits,Callback callback) {
        return owner.create(index,bits,callback);
    }
}
