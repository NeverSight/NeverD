package fixture;
public final class Provider {
    public static Object seenCarrier,seenSpec,first,second,seenUnit;public static Bound seenVector;public static boolean seenFlag;
    public static <T,V extends Bound> Object work(Carrier<T,V> carrier,Spec<T> spec,boolean flag,Action<? super Carrier<T,V>,Unit> action,Completion<? super Unit> completion){
        seenCarrier=carrier;seenSpec=spec;seenFlag=flag;
        T a=carrier.read();first=a;V v=carrier.vector();seenVector=v;T b=spec.read();second=b;
        Unit u=action.apply(carrier);seenUnit=u;completion.accept(u);return flag?a:b;
    }
}
