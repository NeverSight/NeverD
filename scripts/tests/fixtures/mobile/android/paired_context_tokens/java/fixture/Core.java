package fixture;

import java.util.HashMap;

public final class Core {
    private Core() {}
    @SuppressWarnings({"rawtypes","unchecked"})
    public static String run(Object owner,Object name,Object left,Object right) {
        Context context=(Context)owner;
        String key=(String)name;
        Class first=(Class)left;
        Class second=(Class)right;
        Trace.hit(3,context,key,first,second,null);
        HashMap result=Provider.read(context,key,first,second);
        Trace.hit(4,context,key,first,second,result);
        return (String)result.get(key);
    }
    @SuppressWarnings({"rawtypes","unchecked"})
    public static void ignore(Object owner,Object name,Object left,Object right) {
        Context context=(Context)owner;
        String key=(String)name;
        Class first=(Class)left;
        Class second=(Class)right;
        Trace.hit(3,context,key,first,second,null);
        Provider.read(context,key,first,second);
        Trace.hit(4,context,key,first,second,null);
    }
}
