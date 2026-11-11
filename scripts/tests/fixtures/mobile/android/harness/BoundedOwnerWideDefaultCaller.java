import fixture.*;
public final class BoundedOwnerWideDefaultCaller {
    public static long call(DefaultApi receiver,Vector a,Vector b,Vector c) {
        return DefaultApi.DefaultImpls.finish(receiver,a,b,c);
    }
}
