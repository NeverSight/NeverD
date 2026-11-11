package fixture;
public final class Provider {
    public static Object converted,read,callbackResult;
    public static <T,V extends Bound> Object work(Converter<T,V> converter,T first,T second,T third,
        Spec<T> spec,Action<? super T,? super T,Unit> action,Completion<? super Unit> completion) {
        converted=converter.convert(third);T value=spec.read();read=value;
        Unit result=action.apply(first,second);callbackResult=result;completion.accept(result);return result;
    }
}
