import fixture.*;
public final class InheritedOwnerDefaultCaller {
    public static Vector call(GrandApi receiver,Vector a,Vector b,Vector c) {
        return GrandApi.DefaultImpls.finish(receiver,a,b,c);
    }
}
