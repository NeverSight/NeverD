package fixture;

import java.util.function.Predicate;

public final class Core {
    private Core() {}
    @SuppressWarnings({"rawtypes","unchecked"})
    public static Provider run(Object owner,Object name,Object type,Object callback) {
        Provider self=(Provider)owner;
        String key=(String)name;
        Class token=(Class)type;
        Predicate predicate=(Predicate)callback;
        Trace.hit(3,self,key,token,predicate,null);
        Provider result=self.bind(key,token,predicate);
        Trace.hit(4,self,key,token,predicate,result);
        return result;
    }
    @SuppressWarnings({"rawtypes","unchecked"})
    public static void ignore(Object owner,Object name,Object type,Object callback) {
        Provider self=(Provider)owner;
        String key=(String)name;
        Class token=(Class)type;
        Predicate predicate=(Predicate)callback;
        Trace.hit(3,self,key,token,predicate,null);
        self.bind(key,token,predicate);
        Trace.hit(4,self,key,token,predicate,null);
    }
}
