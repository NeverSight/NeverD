package fixture;

public class Core {
    public static interface Selector<E> {
        int rank(E value);
        boolean flag(E value);
    }
    public static class Checker implements Selector<Object> {
        public Checker() {}
        @Override public int rank(Object value) {
            Trace.hit(3,Trace.array,Trace.mode,this,value);
            return Trace.rank;
        }
        @Override public boolean flag(Object value) {
            Trace.hit(4,Trace.array,Trace.mode,this,value);
            return Trace.flag;
        }
    }
    public Core() {}
    public static Object entry(Object values,int mode,Object rule) {
        return choose((Object[])values,mode,(Selector)rule);
    }
    public static <T> T choose(T[] values,int mode,Selector<T> rule) {
        return choose(values,mode,Trace.want,rule);
    }
    public static <T> T choose(T[] values,int mode,boolean wanted,Selector<T> rule) {
        Trace.hit(1,values,mode,rule,null);
        T first=values[Trace.index];
        Trace.hit(2,values,mode,rule,first);
        int rank=rule.rank(first);
        boolean flag=rule.flag(first);
        T result=values[rank+(flag==wanted?0:Trace.delta)];
        if(Trace.override)result=(T)Trace.reply;
        Trace.hit(5,values,mode,rule,result);
        return result;
    }
}
