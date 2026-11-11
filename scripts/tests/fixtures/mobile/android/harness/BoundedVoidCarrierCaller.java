import api.*;
import fixture.*;
public class BoundedVoidCarrierCaller {
    public static void call(Function factory,Marker first,Function reset,Function release,Function update,Marker second,int flags,int mask) {
        Provider.work(factory,first,reset,release,update,second,flags,mask);
    }
}
