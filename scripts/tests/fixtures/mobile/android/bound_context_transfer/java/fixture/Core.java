package fixture;

import java.util.List;

public final class Core {
    private Core() {}
    @SuppressWarnings({"rawtypes","unchecked"})
    public static Bound run(Object owner,Object destination,Object name) {
        Context context=(Context)owner;
        List target=(List)destination;
        String key=(String)name;
        Trace.hit(3,context,target,key,null,null);
        List result=Provider.read(context,target,key);
        Trace.hit(4,context,target,key,result,null);
        return (Bound)result.get(0);
    }
    @SuppressWarnings({"rawtypes","unchecked"})
    public static void ignore(Object owner,Object destination,Object name) {
        Context context=(Context)owner;
        List target=(List)destination;
        String key=(String)name;
        Trace.hit(3,context,target,key,null,null);
        Provider.read(context,target,key);
        Trace.hit(4,context,target,key,null,null);
    }
}
