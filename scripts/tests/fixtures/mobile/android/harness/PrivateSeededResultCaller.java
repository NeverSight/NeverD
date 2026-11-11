import fixture.*;
@SuppressWarnings("rawtypes")
public final class PrivateSeededResultCaller {
    public static Object call(int kind,Core peer,Object seed,Callback callback,Factory producer) {
        if(kind==0)return Core.checked(peer,seed,callback,producer);
        if(kind==1)return Core.raw(peer,seed,callback,producer);
        Core.discard(peer,seed,callback,producer);return null;
    }
}
