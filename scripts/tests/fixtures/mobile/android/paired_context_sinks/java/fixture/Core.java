package fixture;

import java.util.Map;

public final class Core {
    private Core() {}
    @SuppressWarnings({"rawtypes","unchecked"})
    public static String run(Object owner,Object destination,Object name,Object left,Object right) {
        Context context=(Context)owner;
        Map target=(Map)destination;
        String key=(String)name;
        Class first=(Class)left;
        Class second=(Class)right;
        Trace.hit(3,context,target,key,first,second,null,null);
        Provider.fill(context,target,key,first,second);
        Trace.hit(4,context,target,key,first,second,null,null);
        return (String)target.get(key);
    }
    @SuppressWarnings({"rawtypes","unchecked"})
    public static void ignore(Object owner,Object destination,Object name,Object left,Object right) {
        Context context=(Context)owner;
        Map target=(Map)destination;
        String key=(String)name;
        Class first=(Class)left;
        Class second=(Class)right;
        Trace.hit(3,context,target,key,first,second,null,null);
        Provider.fill(context,target,key,first,second);
        Trace.hit(4,context,target,key,first,second,null,null);
    }
}
