package fixture;

public final class Core {
    private Core() {}
    @SuppressWarnings({"rawtypes","unchecked"})
    public static String run(Object owner,Object first,Object second,Object seed,Object auxiliary,int direction) {
        Provider.Pair pair=(Provider.Pair)first;
        Provider.Single single=(Provider.Single)second;
        Context context=(Context)auxiliary;
        Trace.hit(0,owner,pair,single,seed,context,null,direction,0L,false,false);
        Object result=Provider.read(owner,pair,single,seed,context,direction);
        Trace.hit(4,owner,pair,single,seed,context,result,direction,0L,false,false);
        return (String)result;
    }
    @SuppressWarnings({"rawtypes","unchecked"})
    public static String relative(Object owner,Object first,Object second,Object seed,long stamp,int direction,boolean forward,boolean wrap) {
        Provider.Pair pair=(Provider.Pair)first;
        Provider.Single single=(Provider.Single)second;
        Trace.hit(0,owner,pair,single,seed,null,null,direction,stamp,forward,wrap);
        Object result=Provider.relative(owner,pair,single,seed,stamp,direction,forward,wrap);
        Trace.hit(4,owner,pair,single,seed,null,result,direction,stamp,forward,wrap);
        return (String)result;
    }
    @SuppressWarnings({"rawtypes","unchecked"})
    public static void ignore(Object owner,Object first,Object second,Object seed,Object auxiliary,int direction) {
        Provider.Pair pair=(Provider.Pair)first;
        Provider.Single single=(Provider.Single)second;
        Context context=(Context)auxiliary;
        Trace.hit(0,owner,pair,single,seed,context,null,direction,0L,false,false);
        Provider.read(owner,pair,single,seed,context,direction);
        Trace.hit(4,owner,pair,single,seed,context,null,direction,0L,false,false);
    }
    @SuppressWarnings({"rawtypes","unchecked"})
    public static void ignoreRelative(Object owner,Object first,Object second,Object seed,long stamp,int direction,boolean forward,boolean wrap) {
        Provider.Pair pair=(Provider.Pair)first;
        Provider.Single single=(Provider.Single)second;
        Trace.hit(0,owner,pair,single,seed,null,null,direction,stamp,forward,wrap);
        Provider.relative(owner,pair,single,seed,stamp,direction,forward,wrap);
        Trace.hit(4,owner,pair,single,seed,null,null,direction,stamp,forward,wrap);
    }
}
