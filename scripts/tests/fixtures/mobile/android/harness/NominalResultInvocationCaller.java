import fixture.*;
public final class NominalResultInvocationCaller {
    public static Carrier<Value> direct(Provider self,Object token,int index,long stamp,double bits) {
        return self.read(token,index,stamp,bits);
    }
    public static Item checked(Provider self,Object token,int index,long stamp,double bits) {
        return self.<Item>read(token,index,stamp,bits).get();
    }
    public static Carrier<Box> stat(Object token,int index,long stamp,double bits) {
        return Provider.staticRead(token,index,stamp,bits);
    }
}
