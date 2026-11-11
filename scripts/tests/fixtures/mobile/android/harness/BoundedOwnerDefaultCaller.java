import fixture.*;
public final class BoundedOwnerDefaultCaller {
    public static Vector call(DefaultApi receiver,Vector a,Vector b,Vector c) {
        return DefaultApi.DefaultImpls.finish(receiver,a,b,c);
    }
}
