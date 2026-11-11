package fixture;
public final class Provider {
    public static Object seenScope,seenAnimation,seenFrame,first,second,seenUnit;
    public static long seenTime; public static long seenScale; public static Bound vector;
    private static <T,V extends Bound> void work(Carrier<T,V> scope,long time,long scale,Carrier<T,V> animation,Frame<T,V> frame,Action<? super Carrier<T,V>,Unit> action){
        seenScope=scope;seenTime=time;seenScale=scale;seenAnimation=animation;seenFrame=frame;
        T a=animation.read(time,scale);first=a;T b=scope.read(time,scale);second=b;
        frame.write(a,(V)vector);Unit u=action.apply(scope);seenUnit=u;
    }
    public static void access(Carrier scope,long time,long scale,Carrier animation,Frame frame,Action action){work(scope,time,scale,animation,frame,action);}
}
