package fixture;
import api.*;
public class Client {
    public static void run(Function factory,Marker first,Function reset,Function release,Function update,Marker second,int flags,int mask) {
        Provider.work(factory,first,reset,release,update,second,flags,mask);
    }
}
