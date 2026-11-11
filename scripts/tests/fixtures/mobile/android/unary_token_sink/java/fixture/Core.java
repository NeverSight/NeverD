package fixture;

import java.util.List;

public final class Core {
    private Core() {}
    @SuppressWarnings({"rawtypes","unchecked"})
    public static String run(Object owner,Object destination,Object name,Object type) {
        Context context=(Context)owner;
        List target=(List)destination;
        String key=(String)name;
        Class token=(Class)type;
        Trace.hit(3,context,target,key,token,null);
        Provider.fill(context,target,key,token);
        Trace.hit(4,context,target,key,token,null);
        return (String)target.get(0);
    }
    @SuppressWarnings({"rawtypes","unchecked"})
    public static void ignore(Object owner,Object destination,Object name,Object type) {
        Context context=(Context)owner;
        List target=(List)destination;
        String key=(String)name;
        Class token=(Class)type;
        Trace.hit(3,context,target,key,token,null);
        Provider.fill(context,target,key,token);
        Trace.hit(4,context,target,key,token,null);
    }
}
